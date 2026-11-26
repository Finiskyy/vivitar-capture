#include "renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>

static const char* kHLSL = R"HLSL(
Texture2D<float>  TY   : register(t0);
Texture2D<float2> TUV  : register(t1);
Texture2D<float4> TRGB : register(t2);
Texture2D<float2> TMV  : register(t3);   // full-res motion vectors (one per 8x8 block)
Texture2D<float4> THist: register(t4);   // temporal history
Texture2D<float>  TA   : register(t5);   // pyramid level, current frame
Texture2D<float>  TB   : register(t6);   // pyramid level, previous frame
Texture2D<float2> TMVc : register(t7);   // coarser level motion vectors
SamplerState SL : register(s0);
cbuffer CB : register(b0) { float4 A; float4 B; float4 C; float4 D; float4 E; };

struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) {
  V o; float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}

float3 yuv2rgb(float y, float2 c, float yOff, float yScale, float cScale, float m709) {
  y = (y - yOff) * yScale; c = (c - 0.5) * cScale;
  float3 a = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);
  float3 b = float3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x);
  return saturate(m709 > 0.5 ? a : b);
}

// ---- Pass 1: YUV -> RGB + edge-preserving noise/block reduction. A = yOff,yScale,cScale,denoise  B = m709,w,h
static const int2 OFF[12] = { int2(-1,-1), int2(0,-1), int2(1,-1), int2(-1,0), int2(1,0), int2(-1,1), int2(0,1), int2(1,1),
                              int2(-2,0), int2(2,0), int2(0,-2), int2(0,2) };
static const float SW[12] = { .6, .85, .6, .85, .85, .6, .85, .6, .45, .45, .45, .45 };
float3 fetchRGB(int2 p) {
  int2 mx = int2(B.y, B.z) - 1;
  p = clamp(p, int2(0, 0), mx);
  float y = TY.Load(int3(p, 0));
  float2 c = TUV.SampleLevel(SL, (float2(p) + 0.5) / B.yz, 0);
  return yuv2rgb(y, c, A.x, A.y, A.z, B.x);
}
static const float BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
// B.w > 0: the planes come from the deblocker (16-bit), so dither the 8-bit output to avoid banding.
float4 PSConvert(V i) : SV_Target {
  int2 p = int2(i.pos.xy);
  float3 col = fetchRGB(p);
  [branch] if (A.w > 0) {
    float sr = lerp(0.03, 0.17, A.w); float k = 1.0 / (2.0 * sr * sr);
    float3 sum = col; float ws = 1;
    [unroll] for (int n = 0; n < 12; n++) {
      float3 s = fetchRGB(p + OFF[n]); float3 d = s - col;
      float w = SW[n] * exp(-dot(d, d) * k); sum += s * w; ws += w;
    }
    col = sum / ws;
  }
  if (B.w > 0) col = saturate(col + (BAYER[(p.y & 3) * 4 + (p.x & 3)] / 16.0 - 0.46875) / 255.0);
  return float4(col, 1);
}

// ---- JPEG block/ringing removal (shift-averaged DCT thresholding, as in the classic "SPP" post-processor).
// The card compresses every frame as JPEG, so the artifacts are 8x8 DCT blocks. For each of 16 grid shifts, every
// 8x8 block is transformed, its small (quantisation-noise) coefficients are zeroed and it is transformed back;
// averaging the 16 results removes block edges and ringing while keeping real detail.
// A = plane W, H, threshold (x), threshold (y channel / chroma V)
static const float DCT[64] = {
  0.3535534, 0.3535534, 0.3535534, 0.3535534, 0.3535534, 0.3535534, 0.3535534, 0.3535534,
  0.4903926, 0.4157348, 0.2777851, 0.0975452, -0.0975452, -0.2777851, -0.4157348, -0.4903926,
  0.4619398, 0.1913417, -0.1913417, -0.4619398, -0.4619398, -0.1913417, 0.1913417, 0.4619398,
  0.4157348, -0.0975452, -0.4903926, -0.2777851, 0.2777851, 0.4903926, 0.0975452, -0.4157348,
  0.3535534, -0.3535534, -0.3535534, 0.3535534, 0.3535534, -0.3535534, -0.3535534, 0.3535534,
  0.2777851, -0.4903926, 0.0975452, 0.4157348, -0.4157348, -0.0975452, 0.4903926, -0.2777851,
  0.1913417, -0.4619398, 0.4619398, -0.1913417, -0.1913417, 0.4619398, -0.4619398, 0.1913417,
  0.0975452, -0.2777851, 0.4157348, -0.4903926, 0.4903926, -0.4157348, 0.2777851, -0.0975452 };
Texture2D<float2> TPlane : register(t8);
RWTexture2DArray<float>  UOutY  : register(u0);
RWTexture2DArray<float2> UOutUV : register(u1);
groupshared float2 gB[64];
groupshared float2 gT[64];
float2 SppBlock(uint3 gid, uint3 tid, out int2 p) {
  uint z = gid.z;
  int2 origin = int2(gid.xy) * 8 - int2((z & 3) * 2, (z >> 2) * 2);
  p = origin + int2(tid.xy);
  int2 mx = int2(A.xy) - 1;
  uint x = tid.x, y = tid.y;
  gB[y * 8 + x] = TPlane.Load(int3(clamp(p, int2(0, 0), mx), 0));
  GroupMemoryBarrierWithGroupSync();
  float2 s = 0;  // forward, rows: gT[y][k=x]
  [unroll] for (int n = 0; n < 8; n++) s += DCT[x * 8 + n] * gB[y * 8 + n];
  gT[y * 8 + x] = s;
  GroupMemoryBarrierWithGroupSync();
  s = 0;  // forward, columns: coefficient (k=y, kx=x)
  [unroll] for (int n2 = 0; n2 < 8; n2++) s += DCT[y * 8 + n2] * gT[n2 * 8 + x];
  if (x + y > 0) s = abs(s) < A.zw ? 0 : s;  // keep DC, drop small AC coefficients
  gB[y * 8 + x] = s;
  GroupMemoryBarrierWithGroupSync();
  s = 0;  // inverse, columns: gT[n=y][kx=x]
  [unroll] for (int k = 0; k < 8; k++) s += DCT[k * 8 + y] * gB[k * 8 + x];
  gT[y * 8 + x] = s;
  GroupMemoryBarrierWithGroupSync();
  s = 0;  // inverse, rows: pixel (y, n=x)
  [unroll] for (int k2 = 0; k2 < 8; k2++) s += DCT[k2 * 8 + x] * gT[y * 8 + k2];
  return s;
}
[numthreads(8, 8, 1)]
void CSSppY(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
  int2 p; float2 v = SppBlock(gid, tid, p);
  if (all(p >= 0) && all(p < int2(A.xy))) UOutY[uint3(p, gid.z)] = v.x;
}
[numthreads(8, 8, 1)]
void CSSppUV(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
  int2 p; float2 v = SppBlock(gid, tid, p);
  if (all(p >= 0) && all(p < int2(A.xy))) UOutUV[uint3(p, gid.z)] = v;
}
// Averages the 16 shifted reconstructions.
Texture2DArray<float2> TSpp : register(t9);
float2 PSSppResolve(V i) : SV_Target {
  int2 p = int2(i.pos.xy); float2 s = 0;
  [unroll] for (int z = 0; z < 16; z++) s += TSpp.Load(int4(p, z, 0));
  return s / 16.0;
}

// ---- Pass 2: upscale + contrast-adaptive sharpening + colour + before/after split.
// A = viewport (x,y,w,h)  B = srcW,srcH,mode,sharp  C = bri,con,sat,split  D = lineW,yOff,yScale,cScale  E = m709
float lz(float x) { x = abs(x); if (x < 1e-4) return 1; if (x >= 2) return 0; float p = 3.14159265 * x; return 2 * sin(p) * sin(p * 0.5) / (p * p); }
float3 lanczos(float2 uv, float2 sz) {
  float2 pos = uv * sz - 0.5; float2 f = frac(pos); int2 b = int2(floor(pos)); int2 mx = int2(sz) - 1;
  float4 wx = float4(lz(1 + f.x), lz(f.x), lz(1 - f.x), lz(2 - f.x));
  float4 wy = float4(lz(1 + f.y), lz(f.y), lz(1 - f.y), lz(2 - f.y));
  float3 sum = 0;
  [unroll] for (int j = 0; j < 4; j++) {
    float3 row = 0;
    [unroll] for (int k = 0; k < 4; k++) row += TRGB.Load(int3(clamp(b + int2(k - 1, j - 1), int2(0, 0), mx), 0)).rgb * wx[k];
    sum += row * wy[j];
  }
  return sum / (dot(wx, float4(1, 1, 1, 1)) * dot(wy, float4(1, 1, 1, 1)));
}
float3 catrom(float2 uv, float2 sz) {
  float2 sp = uv * sz; float2 t1 = floor(sp - 0.5) + 0.5; float2 f = sp - t1;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2, a = (t1 - 1) / sz, c = (t1 + 2) / sz, m = (t1 + w2 / w12) / sz;
  return TRGB.SampleLevel(SL, float2(a.x, a.y), 0).rgb * w0.x * w0.y + TRGB.SampleLevel(SL, float2(m.x, a.y), 0).rgb * w12.x * w0.y
       + TRGB.SampleLevel(SL, float2(c.x, a.y), 0).rgb * w3.x * w0.y + TRGB.SampleLevel(SL, float2(a.x, m.y), 0).rgb * w0.x * w12.y
       + TRGB.SampleLevel(SL, float2(m.x, m.y), 0).rgb * w12.x * w12.y + TRGB.SampleLevel(SL, float2(c.x, m.y), 0).rgb * w3.x * w12.y
       + TRGB.SampleLevel(SL, float2(a.x, c.y), 0).rgb * w0.x * w3.y + TRGB.SampleLevel(SL, float2(m.x, c.y), 0).rgb * w12.x * w3.y
       + TRGB.SampleLevel(SL, float2(c.x, c.y), 0).rgb * w3.x * w3.y;
}
float4 PSMain(V i) : SV_Target {
  float2 uv = i.uv;
  float4 o;
  [branch] if (C.w >= 0 && i.pos.x < C.w) {
    float y = TY.SampleLevel(SL, uv, 0); float2 ch = TUV.SampleLevel(SL, uv, 0);
    o = float4(yuv2rgb(y, ch, D.y, D.z, D.w, E.x), 1);
  } else {
    float2 sz = B.xy; int mode = int(B.z + 0.5);
    float3 c;
    [branch] if (mode == 2) c = lanczos(uv, sz);
    else if (mode == 1) c = catrom(uv, sz);
    else c = TRGB.SampleLevel(SL, uv, 0).rgb;
    c = saturate(c);
    float3 col = c;
    [branch] if (B.w > 0) {
      float2 t = 1.0 / sz;
      float3 n = TRGB.SampleLevel(SL, uv - float2(0, t.y), 0).rgb, s = TRGB.SampleLevel(SL, uv + float2(0, t.y), 0).rgb;
      float3 e = TRGB.SampleLevel(SL, uv + float2(t.x, 0), 0).rgb, w = TRGB.SampleLevel(SL, uv - float2(t.x, 0), 0).rgb;
      float3 mn = min(c, min(min(n, s), min(w, e))), mx = max(c, max(max(n, s), max(w, e)));
      float3 amp = sqrt(saturate(min(mn, 1 - mx) / max(mx, 1e-4)));
      float3 wt = amp * (-1.0 / lerp(8.0, 4.5, B.w));
      col = saturate((c + (n + s + w + e) * wt) / (1 + 4 * wt));
    }
    col = (col - 0.5) * C.y + 0.5 + C.x;
    float l = dot(col, float3(0.2126, 0.7152, 0.0722));
    o = float4(saturate(lerp(l.xxx, col, C.z)), 1);
  }
  if (E.y > 0.5) {  // debug: colour by motion direction, brightness by speed
    float2 mv = TMV.SampleLevel(SL, uv, 0);
    float m = saturate(length(mv) / 12.0);
    float a = atan2(mv.y, mv.x) / 6.2831853 + 0.5;
    float3 hue = saturate(abs(frac(a + float3(0, 2.0 / 3, 1.0 / 3)) * 6 - 3) - 1);
    o.rgb = lerp(o.rgb, hue, m * 0.75);
  }
  if (C.w >= 0 && abs(i.pos.x - C.w) < D.x) o = 1;
  return o;
}

// ---- Motion estimation: hierarchical 8x8 block matching (backward vectors: prev(x + mv) ~= cur(x)).
// Down: A.xy = source level size.
float PSDown(V i) : SV_Target {
  int2 p = int2(i.pos.xy) * 2; int2 mx = int2(A.xy) - 1;
  return 0.25 * (TA.Load(int3(min(p, mx), 0)) + TA.Load(int3(min(p + int2(1, 0), mx), 0)) +
                 TA.Load(int3(min(p + int2(0, 1), mx), 0)) + TA.Load(int3(min(p + int2(1, 1), mx), 0)));
}
// Motion: A = levelW, levelH, search radius, hasCoarse   B = coarseMvW, coarseMvH, halfPel
float SADb(int2 o, float2 mv) {
  float s = 0; int2 mx = int2(A.xy) - 1; float2 inv = 1.0 / A.xy;
  [unroll] for (int y = 0; y < 8; y++) {
    [unroll] for (int x = 0; x < 8; x++) {
      int2 p = min(o + int2(x, y), mx);
      s += abs(TA.Load(int3(p, 0)) - TB.SampleLevel(SL, (float2(p) + 0.5 + mv) * inv, 0));
    }
  }
  return s;
}
static const int2 NB[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
static const float2 HP[8] = { float2(-.5, -.5), float2(0, -.5), float2(.5, -.5), float2(-.5, 0),
                              float2(.5, 0), float2(-.5, .5), float2(0, .5), float2(.5, .5) };
float2 PSMotion(V i) : SV_Target {
  int2 bl = int2(i.pos.xy); int2 o = bl * 8;
  float2 best = 0; float bestS = SADb(o, float2(0, 0)) * 0.97 - 0.02;  // ties go to "no motion" (noise)
  if (A.w > 0.5) {
    int2 cb = bl >> 1; int2 cmx = int2(B.xy) - 1;
    [unroll] for (int k = 0; k < 5; k++) {
      float2 pr = TMVc.Load(int3(clamp(cb + NB[k], int2(0, 0), cmx), 0)) * 2;
      float sc = SADb(o, pr);
      if (sc < bestS) { bestS = sc; best = pr; }
    }
  }
  int r = int(A.z); float2 c0 = best;
  [loop] for (int dy = -r; dy <= r; dy++) {
    [loop] for (int dx = -r; dx <= r; dx++) {
      float2 pr = c0 + float2(dx, dy);
      float sc = SADb(o, pr);
      if (sc < bestS) { bestS = sc; best = pr; }
    }
  }
  if (B.z > 0.5) {
    float2 c1 = best;
    [loop] for (int k2 = 0; k2 < 8; k2++) {
      float2 pr = c1 + HP[k2];
      float sc = SADb(o, pr);
      if (sc < bestS) { bestS = sc; best = pr; }
    }
  }
  return best;
}
// ---- Motion-compensated temporal noise reduction. A = W, H, strength, historyValid
float4 PSTemporal(V i) : SV_Target {
  int2 p = int2(i.pos.xy); int2 mx = int2(A.xy) - 1;
  float3 c = TRGB.Load(int3(p, 0)).rgb;
  if (A.w < 0.5) return float4(c, 1);
  float2 mv = TMV.SampleLevel(SL, (float2(p) + 0.5) / A.xy, 0);
  float2 hu = (float2(p) + 0.5 + mv) / A.xy;
  if (any(hu < 0) || any(hu > 1)) return float4(c, 1);
  float3 h = THist.SampleLevel(SL, hu, 0).rgb;
  float3 mn = c, mxc = c;
  [unroll] for (int y = -1; y <= 1; y++) {
    [unroll] for (int x = -1; x <= 1; x++) {
      float3 n = TRGB.Load(int3(clamp(p + int2(x, y), int2(0, 0), mx), 0)).rgb;
      mn = min(mn, n); mxc = max(mxc, n);
    }
  }
  float3 pad = 0.02 + (mxc - mn) * 0.15;           // allow noise-sized differences, not real changes
  float3 hc = clamp(h, mn - pad, mxc + pad);
  float3 d = h - c;
  float w = A.z * exp(-dot(d, d) * 30.0);          // trust history less where it disagrees (bad match)
  return float4(lerp(c, hc, w), 1);
}
// Per-pixel motion vectors for FSR (pixel units, pointing to the previous frame). A = W, H, valid
float2 PSMVUp(V i) : SV_Target {
  if (A.z < 0.5) return float2(0, 0);
  return TMV.SampleLevel(SL, i.pos.xy / A.xy, 0);
}
float4 PSCopy(V i) : SV_Target { return float4(THist.Load(int3(int2(i.pos.xy), 0)).rgb, 1); }

// ---- RGB -> NV12 (BT.709 limited range) for the encoder.
float PSToY(V i) : SV_Target {
  float3 c = TRGB.Load(int3(int2(i.pos.xy), 0)).rgb;
  return 16.0 / 255 + dot(c, float3(0.2126, 0.7152, 0.0722)) * 219.0 / 255;
}
float2 PSToUV(V i) : SV_Target {
  int2 p = int2(i.pos.xy) * 2;
  float3 c = (TRGB.Load(int3(p, 0)).rgb + TRGB.Load(int3(p + int2(1, 0), 0)).rgb +
              TRGB.Load(int3(p + int2(0, 1), 0)).rgb + TRGB.Load(int3(p + int2(1, 1), 0)).rgb) * 0.25;
  float u = dot(c, float3(-0.1146, -0.3854, 0.5)), v = dot(c, float3(0.5, -0.4542, -0.0458));
  return float2(u, v) * 224.0 / 255 + 128.0 / 255;
}
)HLSL";

struct CBData {
  float A[4], B[4], C[4], D[4], E[4];
};

static HRESULT Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err) {
  ComPtr<ID3DBlob> errs;
  HRESULT hr = D3DCompile(kHLSL, strlen(kHLSL), "vivitar.hlsl", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errs);
  if (FAILED(hr)) {
    err = std::string("Shader ") + entry + " failed: " + (errs ? (const char*)errs->GetBufferPointer() : HrStr(hr));
    Log("%s", err.c_str());
  }
  return hr;
}

bool Renderer::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string& err) {
  dev_ = dev;
  ctx_ = ctx;
  ComPtr<ID3DBlob> b;
  if (FAILED(Compile("VSMain", "vs_5_0", b, err))) return false;
  dev_->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_);
  struct {
    const char* e;
    ComPtr<ID3D11PixelShader>* ps;
  } list[] = {{"PSConvert", &psConvert_}, {"PSMain", &psMain_}, {"PSToY", &psY_},         {"PSToUV", &psUV_},
              {"PSDown", &psDown_},       {"PSMotion", &psMotion_}, {"PSTemporal", &psTemporal_}, {"PSCopy", &psCopy_},
              {"PSMVUp", &psMVUp_}};
  for (auto& l : list) {
    std::string e2;
    if (FAILED(Compile(l.e, "ps_5_0", b, e2))) {
      // The motion/temporal shaders are optional extras; the core ones are required.
      bool optional = !strcmp(l.e, "PSDown") || !strcmp(l.e, "PSMotion") || !strcmp(l.e, "PSTemporal") ||
                      !strcmp(l.e, "PSCopy") || !strcmp(l.e, "PSMVUp");
      if (!optional) {
        err = e2;
        return false;
      }
      motionOk_ = false;
      continue;
    }
    dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, l.ps->GetAddressOf());
  }
  // JPEG deblocking: optional too (without it Convert falls back to the spatial filter alone).
  {
    std::string e2;
    ComPtr<ID3DBlob> by, buv, br;
    if (SUCCEEDED(Compile("CSSppY", "cs_5_0", by, e2)) && SUCCEEDED(Compile("CSSppUV", "cs_5_0", buv, e2)) &&
        SUCCEEDED(Compile("PSSppResolve", "ps_5_0", br, e2)) &&
        SUCCEEDED(dev_->CreateComputeShader(by->GetBufferPointer(), by->GetBufferSize(), nullptr, &csSppY_)) &&
        SUCCEEDED(dev_->CreateComputeShader(buv->GetBufferPointer(), buv->GetBufferSize(), nullptr, &csSppUV_)) &&
        SUCCEEDED(dev_->CreatePixelShader(br->GetBufferPointer(), br->GetBufferSize(), nullptr, &psSppResolve_)))
      sppOk_ = true;
    else {
      sppOk_ = false;
      Log("JPEG deblocking unavailable");
    }
  }
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(CBData);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  dev_->CreateBuffer(&bd, nullptr, &cb_);
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  dev_->CreateSamplerState(&sd, &samp_);
  return true;
}

bool Renderer::Ensure(RenderTarget& t, UINT w, UINT h, DXGI_FORMAT fmt) {
  w = std::max<UINT>(1, w);
  h = std::max<UINT>(1, h);
  if (t.tex && t.w == w && t.h == h && t.fmt == fmt) return false;
  t = RenderTarget{};
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &t.tex))) return false;
  dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
  dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
  t.w = w;
  t.h = h;
  t.fmt = fmt;
  return true;
}

void Renderer::Upload(const VideoFrame& f) {
  if (!f.w || !f.h || f.data.size() < size_t(f.w) * f.h * 3 / 2) return;
  if (!texY_ || f.w != srcW || f.h != srcH) {
    texY_.Reset();
    texUV_.Reset();
    srvY_.Reset();
    srvUV_.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = f.w;
    d.Height = f.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev_->CreateTexture2D(&d, nullptr, &texY_);
    d.Width = f.w / 2;
    d.Height = f.h / 2;
    d.Format = DXGI_FORMAT_R8G8_UNORM;
    dev_->CreateTexture2D(&d, nullptr, &texUV_);
    dev_->CreateShaderResourceView(texY_.Get(), nullptr, &srvY_);
    dev_->CreateShaderResourceView(texUV_.Get(), nullptr, &srvUV_);
    srcW = f.w;
    srcH = f.h;
  }
  ctx_->UpdateSubresource(texY_.Get(), 0, nullptr, f.data.data(), f.w, 0);
  ctx_->UpdateSubresource(texUV_.Get(), 0, nullptr, f.data.data() + size_t(f.w) * f.h, f.w, 0);
  uploadSerial_++;
}

void Renderer::Pass(ID3D11RenderTargetView* rtv, float x, float y, float w, float h, ID3D11PixelShader* ps,
                    const void* cb, size_t cbSize, std::initializer_list<ID3D11ShaderResourceView*> srvs) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, cb, cbSize);
    ctx_->Unmap(cb_.Get(), 0);
  }
  ID3D11ShaderResourceView* nulls[kSlots] = {};
  ctx_->PSSetShaderResources(0, kSlots, nulls);
  ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(vs_.Get(), nullptr, 0);
  ctx_->PSSetShader(ps, nullptr, 0);
  ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
  ID3D11ShaderResourceView* s[kSlots] = {};
  int i = 0;
  for (auto* v : srvs)
    if (i < kSlots) s[i++] = v;
  ctx_->PSSetShaderResources(0, kSlots, s);
  ctx_->Draw(3, 0);
  ctx_->PSSetShaderResources(0, kSlots, nulls);
  ID3D11RenderTargetView* nrt = nullptr;
  ctx_->OMSetRenderTargets(1, &nrt, nullptr);
}

static void RangeParams(const ColorParams& c, float& yOff, float& yScale, float& cScale) {
  if (c.fullRange) {
    yOff = 0;
    yScale = 1;
    cScale = 1;
  } else {
    yOff = 16.f / 255.f;
    yScale = 255.f / 219.f;
    cScale = 255.f / 224.f;
  }
}

// Creates a 16-slice array (one slice per grid shift) with UAV + SRV, for the deblocker.
static bool MakeSppArray(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex,
                         ComPtr<ID3D11UnorderedAccessView>& uav, ComPtr<ID3D11ShaderResourceView>& srv) {
  D3D11_TEXTURE2D_DESC d{};
  if (tex) {
    tex->GetDesc(&d);
    if (d.Width == w && d.Height == h) return true;
  }
  tex.Reset();
  uav.Reset();
  srv.Reset();
  d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 16;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &tex)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav)) &&
         SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv));
}

bool Renderer::Deblock(float threshold) {
  if (!sppOk_ || srcW < 16 || srcH < 16) return false;
  UINT cw = srcW / 2, ch = srcH / 2;
  if (!MakeSppArray(dev_, srcW, srcH, DXGI_FORMAT_R16_FLOAT, sppArrY_, sppUavY_, sppSrvY_) ||
      !MakeSppArray(dev_, cw, ch, DXGI_FORMAT_R16G16_FLOAT, sppArrUV_, sppUavUV_, sppSrvUV_)) {
    Log("JPEG deblocking: couldn't allocate its textures");
    sppOk_ = false;
    return false;
  }
  bool fresh = Ensure(sppY_, srcW, srcH, DXGI_FORMAT_R16_FLOAT);
  fresh |= Ensure(sppUV_, cw, ch, DXGI_FORMAT_R16G16_FLOAT);
  if (!fresh && sppSerial_ == uploadSerial_ && sppT_ == threshold) return true;  // this picture is done already
  sppSerial_ = uploadSerial_;
  sppT_ = threshold;

  auto run = [&](ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, UINT slot,
                 UINT w, UINT h, float tx, float ty) {
    CBData cb{};
    cb.A[0] = float(w);
    cb.A[1] = float(h);
    cb.A[2] = tx;
    cb.A[3] = ty;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      memcpy(m.pData, &cb, sizeof(cb));
      ctx_->Unmap(cb_.Get(), 0);
    }
    ctx_->CSSetShader(cs, nullptr, 0);
    ctx_->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->CSSetShaderResources(8, 1, &in);
    ctx_->CSSetUnorderedAccessViews(slot, 1, &out, nullptr);
    ctx_->Dispatch(w / 8 + 1, h / 8 + 1, 16);  // +1: shifted grids start up to 6 pixels before the edge
    ID3D11ShaderResourceView* ns = nullptr;
    ID3D11UnorderedAccessView* nu = nullptr;
    ctx_->CSSetShaderResources(8, 1, &ns);
    ctx_->CSSetUnorderedAccessViews(slot, 1, &nu, nullptr);
    ctx_->CSSetShader(nullptr, nullptr, 0);
  };
  // Chroma carries coarser, stronger artifacts and little fine detail, so it gets a higher threshold.
  run(csSppY_.Get(), srvY_.Get(), sppUavY_.Get(), 0, srcW, srcH, threshold, threshold);
  run(csSppUV_.Get(), srvUV_.Get(), sppUavUV_.Get(), 1, cw, ch, threshold * 1.5f, threshold * 1.5f);

  CBData cb{};
  ID3D11ShaderResourceView* none = nullptr;
  Pass(sppY_.rtv.Get(), 0, 0, float(srcW), float(srcH), psSppResolve_.Get(), &cb, sizeof(cb),
       {none, none, none, none, none, none, none, none, none, sppSrvY_.Get()});
  Pass(sppUV_.rtv.Get(), 0, 0, float(cw), float(ch), psSppResolve_.Get(), &cb, sizeof(cb),
       {none, none, none, none, none, none, none, none, none, sppSrvUV_.Get()});
  return true;
}

void Renderer::Convert(const ColorParams& c, float denoise) {
  if (!HasFrame()) return;
  Ensure(rgb_, srcW, srcH, DXGI_FORMAT_R8G8B8A8_UNORM);
  // The card's JPEG compression is the main artifact, so block/ringing removal does most of the work and the
  // edge-preserving spatial filter only mops up what's left.
  bool deblocked = denoise > 0 && Deblock(0.01f + 0.06f * denoise);
  CBData cb{};
  RangeParams(c, cb.A[0], cb.A[1], cb.A[2]);
  cb.A[3] = deblocked ? denoise * 0.4f : denoise;
  cb.B[0] = c.bt709 ? 1.f : 0.f;
  cb.B[1] = float(srcW);
  cb.B[2] = float(srcH);
  cb.B[3] = deblocked ? 1.f : 0.f;
  Pass(rgb_.rtv.Get(), 0, 0, float(srcW), float(srcH), psConvert_.Get(), &cb, sizeof(cb),
       {deblocked ? sppY_.srv.Get() : srvY_.Get(), deblocked ? sppUV_.srv.Get() : srvUV_.Get(), nullptr});
}

void Renderer::FitRect(UINT tw, UINT th, int fit, float& x, float& y, float& w, float& h) const {
  float cw = float(tw), ch = float(th), vw = float(std::max<UINT>(1, srcW)), vh = float(std::max<UINT>(1, srcH));
  x = 0, y = 0, w = cw, h = ch;
  if (fit == FitStretch) return;
  if (fit == FitNative) {
    w = vw;
    h = vh;
  } else {
    float s = fit == FitCover ? std::max(cw / vw, ch / vh) : std::min(cw / vw, ch / vh);
    w = std::round(vw * s);
    h = std::round(vh * s);
  }
  x = std::round((cw - w) / 2);
  y = std::round((ch - h) / 2);
}

bool Renderer::CopyToOwned(ID3D11Texture2D* src, RenderTarget& dst) {
  if (!src) return false;
  D3D11_TEXTURE2D_DESC sd;
  src->GetDesc(&sd);
  if (!dst.tex || dst.w != sd.Width || dst.h != sd.Height || dst.fmt != sd.Format) {
    dst = RenderTarget{};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = sd.Width;
    d.Height = sd.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = sd.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &dst.tex))) return false;
    // Typeless textures (AMF sometimes allocates these) need an explicit view format.
    DXGI_FORMAT vf = sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                     : sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                  : DXGI_FORMAT_UNKNOWN;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = vf;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srd{};
    srd.Format = vf;
    srd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srd.Texture2D.MipLevels = 1;
    dev_->CreateRenderTargetView(dst.tex.Get(), vf != DXGI_FORMAT_UNKNOWN ? &rd : nullptr, &dst.rtv);
    dev_->CreateShaderResourceView(dst.tex.Get(), vf != DXGI_FORMAT_UNKNOWN ? &srd : nullptr, &dst.srv);
    dst.w = sd.Width;
    dst.h = sd.Height;
    dst.fmt = sd.Format;
  }
  if (sd.ArraySize > 1 || sd.MipLevels > 1) {
    // AMF pools frames in texture arrays and tags each texture with the slice in use.
    static const GUID kAmfArrayIndex = {0x28115527, 0xe7c3, 0x4b66, {0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf}};
    int slice = 0;
    UINT sz = sizeof(slice);
    if (FAILED(src->GetPrivateData(kAmfArrayIndex, &sz, &slice))) slice = 0;
    ctx_->CopySubresourceRegion(dst.tex.Get(), 0, 0, 0, 0, src, D3D11CalcSubresource(0, slice, sd.MipLevels), nullptr);
  } else
    ctx_->CopyResource(dst.tex.Get(), src);
  return true;
}

void Renderer::Draw(RenderTarget& t, int fit, const EnhanceParams& e, const ColorParams& c, float splitPx, float lineW,
                    const SrcOverride* ov) {
  if (!t.rtv) return;
  float black[4] = {0, 0, 0, 1};
  ctx_->ClearRenderTargetView(t.rtv.Get(), black);
  if (!HasFrame() || !rgb_.srv) return;
  float x, y, w, h;
  FitRect(t.w, t.h, fit, x, y, w, h);
  SrcOverride base = Rgb();
  ID3D11ShaderResourceView* src = ov && ov->srv ? ov->srv : base.srv;
  float vw = ov && ov->srv ? float(ov->w) : float(srcW), vh = ov && ov->srv ? float(ov->h) : float(srcH);
  CBData cb{};
  cb.A[0] = x;
  cb.A[1] = y;
  cb.A[2] = w;
  cb.A[3] = h;
  cb.B[0] = vw;
  cb.B[1] = vh;
  cb.B[2] = float(e.scaler);
  cb.B[3] = e.sharpen;
  cb.C[0] = e.bri;
  cb.C[1] = e.con;
  cb.C[2] = e.sat;
  cb.C[3] = splitPx;
  cb.D[0] = lineW;
  RangeParams(c, cb.D[1], cb.D[2], cb.D[3]);
  cb.E[0] = c.bt709 ? 1.f : 0.f;
  bool mvOverlay = showMotion && haveMotion_ && mv_[0].srv;
  cb.E[1] = mvOverlay ? 1.f : 0.f;
  Pass(t.rtv.Get(), x, y, w, h, psMain_.Get(), &cb, sizeof(cb),
       {srvY_.Get(), srvUV_.Get(), src, mvOverlay ? mv_[0].srv.Get() : nullptr});
}

void Renderer::Motion() {
  if (!HasFrame() || !texY_ || !motionOk_) return;
  std::swap(cur_, prev_);
  UINT lw[4], lh[4];
  bool sizeChanged = false;
  for (int l = 0; l < 4; l++) {
    lw[l] = std::max<UINT>(8, srcW >> l);
    lh[l] = std::max<UINT>(8, srcH >> l);
    Ensure(cur_[l], lw[l], lh[l], DXGI_FORMAT_R8_UNORM);
    if (prev_[l].w != lw[l] || prev_[l].h != lh[l]) sizeChanged = true;
  }
  ctx_->CopyResource(cur_[0].tex.Get(), texY_.Get());
  CBData cb{};
  for (int l = 1; l < 4; l++) {
    cb.A[0] = float(lw[l - 1]);
    cb.A[1] = float(lh[l - 1]);
    Pass(cur_[l].rtv.Get(), 0, 0, float(lw[l]), float(lh[l]), psDown_.Get(), &cb, sizeof(cb),
         {nullptr, nullptr, nullptr, nullptr, nullptr, cur_[l - 1].srv.Get()});
  }
  if (sizeChanged || !havePrevPyr_) {
    havePrevPyr_ = true;
    haveMotion_ = false;
    return;
  }
  for (int l = 3; l >= 0; l--) {
    UINT mw = (lw[l] + 7) / 8, mh = (lh[l] + 7) / 8;
    Ensure(mv_[l], mw, mh, DXGI_FORMAT_R16G16_FLOAT);
    cb = {};
    cb.A[0] = float(lw[l]);
    cb.A[1] = float(lh[l]);
    cb.A[2] = l == 3 ? 6.f : 1.f;  // wide search at the coarsest level, small refinements above it
    cb.A[3] = l < 3 ? 1.f : 0.f;
    if (l < 3) {
      cb.B[0] = float(mv_[l + 1].w);
      cb.B[1] = float(mv_[l + 1].h);
    }
    cb.B[2] = l == 0 ? 1.f : 0.f;  // half-pixel refinement at full resolution
    Pass(mv_[l].rtv.Get(), 0, 0, float(mw), float(mh), psMotion_.Get(), &cb, sizeof(cb),
         {nullptr, nullptr, nullptr, nullptr, nullptr, cur_[l].srv.Get(), prev_[l].srv.Get(),
          l < 3 ? mv_[l + 1].srv.Get() : nullptr});
  }
  haveMotion_ = true;
}

void Renderer::Temporal(float strength) {
  if (!HasFrame() || strength <= 0 || !motionOk_) {
    temporalOut_ = false;
    haveHist_ = false;
    return;
  }
  bool re = Ensure(hist_[0], srcW, srcH, DXGI_FORMAT_R16G16B16A16_FLOAT);
  re |= Ensure(hist_[1], srcW, srcH, DXGI_FORMAT_R16G16B16A16_FLOAT);
  Ensure(rgbT_, srcW, srcH, DXGI_FORMAT_R8G8B8A8_UNORM);
  if (re) haveHist_ = false;
  RenderTarget& prevH = hist_[histIdx_];
  RenderTarget& nextH = hist_[histIdx_ ^ 1];
  CBData cb{};
  cb.A[0] = float(srcW);
  cb.A[1] = float(srcH);
  cb.A[2] = strength;
  cb.A[3] = (haveHist_ && haveMotion_) ? 1.f : 0.f;
  Pass(nextH.rtv.Get(), 0, 0, float(srcW), float(srcH), psTemporal_.Get(), &cb, sizeof(cb),
       {nullptr, nullptr, rgb_.srv.Get(), haveMotion_ ? mv_[0].srv.Get() : nullptr, prevH.srv.Get()});
  Pass(rgbT_.rtv.Get(), 0, 0, float(srcW), float(srcH), psCopy_.Get(), &cb, sizeof(cb),
       {nullptr, nullptr, nullptr, nullptr, nextH.srv.Get()});
  histIdx_ ^= 1;
  haveHist_ = true;
  temporalOut_ = true;
}

void Renderer::ResetNV12() {
  busy_.clear();
  free_.clear();
  slots_.clear();
  nvY_ = RenderTarget{};
  nvUV_ = RenderTarget{};
}

void Renderer::SubmitNV12(RenderTarget& src, int64_t t) {
  UINT w = src.w, h = src.h;
  if (Ensure(nvY_, w, h, DXGI_FORMAT_R8_UNORM) | Ensure(nvUV_, w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM)) {
    slots_.clear();
    busy_.clear();
    free_.clear();
  }
  if (slots_.empty()) {
    slots_.resize(4);
    for (int i = 0; i < 4; i++) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_STAGING;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      dev_->CreateTexture2D(&d, nullptr, &slots_[i].y);
      d.Width = w / 2;
      d.Height = h / 2;
      d.Format = DXGI_FORMAT_R8G8_UNORM;
      dev_->CreateTexture2D(&d, nullptr, &slots_[i].uv);
      free_.push_back(i);
    }
  }
  CBData cb{};
  Pass(nvY_.rtv.Get(), 0, 0, float(w), float(h), psY_.Get(), &cb, sizeof(cb), {nullptr, nullptr, src.srv.Get()});
  Pass(nvUV_.rtv.Get(), 0, 0, float(w / 2), float(h / 2), psUV_.Get(), &cb, sizeof(cb), {nullptr, nullptr, src.srv.Get()});
  if (free_.empty()) return;  // caller collects every frame, so this shouldn't happen
  int i = free_.back();
  free_.pop_back();
  ctx_->CopyResource(slots_[i].y.Get(), nvY_.tex.Get());
  ctx_->CopyResource(slots_[i].uv.Get(), nvUV_.tex.Get());
  slots_[i].t = t;
  busy_.push_back(i);
}

void Renderer::CollectNV12(const std::function<void(const uint8_t*, size_t, int64_t)>& sink, bool flushAll) {
  while (!busy_.empty()) {
    if (!flushAll && busy_.size() < 3) break;  // let the GPU run two frames ahead
    int i = busy_.front();
    busy_.pop_front();
    UINT w = nvY_.w, h = nvY_.h;
    packed_.resize(size_t(w) * h * 3 / 2);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = true;
    if (SUCCEEDED(ctx_->Map(slots_[i].y.Get(), 0, D3D11_MAP_READ, 0, &m))) {
      for (UINT r = 0; r < h; r++) memcpy(packed_.data() + size_t(r) * w, (uint8_t*)m.pData + size_t(r) * m.RowPitch, w);
      ctx_->Unmap(slots_[i].y.Get(), 0);
    } else ok = false;
    if (ok && SUCCEEDED(ctx_->Map(slots_[i].uv.Get(), 0, D3D11_MAP_READ, 0, &m))) {
      uint8_t* dst = packed_.data() + size_t(w) * h;
      for (UINT r = 0; r < h / 2; r++) memcpy(dst + size_t(r) * w, (uint8_t*)m.pData + size_t(r) * m.RowPitch, w);
      ctx_->Unmap(slots_[i].uv.Get(), 0);
    } else ok = false;
    free_.push_back(i);
    if (ok) sink(packed_.data(), packed_.size(), slots_[i].t);
  }
}

bool Renderer::ReadRGBA(RenderTarget& src, std::vector<uint8_t>& out) {
  D3D11_TEXTURE2D_DESC d{};
  if (rgbaStaging_) rgbaStaging_->GetDesc(&d);
  if (!rgbaStaging_ || d.Width != src.w || d.Height != src.h) {
    rgbaStaging_.Reset();
    d = {};
    d.Width = src.w;
    d.Height = src.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &rgbaStaging_))) return false;
  }
  ctx_->CopyResource(rgbaStaging_.Get(), src.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(rgbaStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  out.resize(size_t(src.w) * src.h * 4);
  for (UINT r = 0; r < src.h; r++)
    memcpy(out.data() + size_t(r) * src.w * 4, (uint8_t*)m.pData + size_t(r) * m.RowPitch, size_t(src.w) * 4);
  ctx_->Unmap(rgbaStaging_.Get(), 0);
  return true;
}

void Renderer::DrawMotionFull(ID3D11RenderTargetView* rtv, UINT w, UINT h) {
  if (!rtv) return;
  if (!psMVUp_ || !haveMotion_ || !mv_[0].srv) {
    float z[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(rtv, z);
    return;
  }
  CBData cb{};
  cb.A[0] = float(w);
  cb.A[1] = float(h);
  cb.A[2] = 1.f;
  Pass(rtv, 0, 0, float(w), float(h), psMVUp_.Get(), &cb, sizeof(cb), {nullptr, nullptr, nullptr, mv_[0].srv.Get()});
}
