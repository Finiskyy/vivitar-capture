#include "amd_fx.h"

#include <algorithm>
#include <deque>

#include "components/FRC.h"
#include "components/HQScaler.h"
#include "components/VideoConverter.h"
#include "components/VideoEncoderVCE.h"
#include "core/Buffer.h"
#include "core/Factory.h"

static std::string AmfErr(AMF_RESULT r) { return "AMF error " + std::to_string((int)r); }

struct AmdFx::Impl {
  HMODULE lib = nullptr;
  amf::AMFFactory* factory = nullptr;
  amf::AMFContextPtr ctx;
  // FRC
  amf::AMFComponentPtr frc;
  UINT frcW = 0, frcH = 0;
  bool frcFailed = false;
  amf::AMFSurfacePtr held[2];
  // HQ scaler slots
  struct Scaler {
    amf::AMFComponentPtr c;
    UINT inW = 0, inH = 0, outW = 0, outH = 0;
    int algo = -1;
    float sharp = -1;
    amf::AMFSurfacePtr held;
  } sc[2];
  // Encoder
  amf::AMFComponentPtr conv, enc;
  std::vector<EncodedPacket> pending;
  std::deque<int64_t> ptsQ;  // our timestamps in submission order

  void TerminateAll() {
    held[0] = nullptr;
    held[1] = nullptr;
    if (frc) frc->Terminate();
    frc = nullptr;
    for (auto& s : sc) {
      s.held = nullptr;
      if (s.c) s.c->Terminate();
      s.c = nullptr;
    }
    if (conv) conv->Terminate();
    if (enc) enc->Terminate();
    conv = nullptr;
    enc = nullptr;
    if (ctx) ctx->Terminate();
    ctx = nullptr;
  }
};

AmdFx::AmdFx() : p_(std::make_unique<Impl>()) {}

AmdFx::~AmdFx() {
  p_->TerminateAll();
  if (p_->lib) FreeLibrary(p_->lib);
}

bool AmdFx::Init(ID3D11Device* dev, UINT vendorId) {
  if (vendorId != 0x1002) {
    status = "Needs an AMD Radeon graphics card";
    return false;
  }
  p_->lib = LoadLibraryW(AMF_DLL_NAME);
  if (!p_->lib) {
    status = "AMD media runtime not found. Update your AMD Adrenalin driver.";
    Log("AMF: %ls not found", AMF_DLL_NAME);
    return false;
  }
  auto queryVersion = (AMFQueryVersion_Fn)GetProcAddress(p_->lib, AMF_QUERY_VERSION_FUNCTION_NAME);
  auto init = (AMFInit_Fn)GetProcAddress(p_->lib, AMF_INIT_FUNCTION_NAME);
  amf_uint64 ver = 0;
  if (!queryVersion || !init || queryVersion(&ver) != AMF_OK) {
    status = "AMD media runtime is unusable. Update your AMD Adrenalin driver.";
    return false;
  }
  char vb[64];
  snprintf(vb, sizeof(vb), "%u.%u.%u", (unsigned)AMF_GET_MAJOR_VERSION(ver), (unsigned)AMF_GET_MINOR_VERSION(ver),
           (unsigned)AMF_GET_SUBMINOR_VERSION(ver));
  version = vb;
  AMF_RESULT r = init(AMF_FULL_VERSION, &p_->factory);
  if (r != AMF_OK || !p_->factory) {
    status = "AMD media runtime failed to start (" + AmfErr(r) + ").";
    return false;
  }
  r = p_->factory->CreateContext(&p_->ctx);
  if (r == AMF_OK) r = p_->ctx->InitDX11(dev, amf::AMF_DX11_0);
  if (r != AMF_OK) {
    status = "AMD media runtime couldn't attach to Direct3D 11 (" + AmfErr(r) + ").";
    p_->ctx = nullptr;
    return false;
  }
  Log("AMF runtime %s ready", vb);
  status = "AMD AMF " + version;
  ready_ = true;
  return true;
}

void AmdFx::FrcReset() {
  if (!ready_) return;
  p_->held[0] = nullptr;
  p_->held[1] = nullptr;
  if (p_->frc) p_->frc->Terminate();
  p_->frc = nullptr;
  p_->frcW = p_->frcH = 0;
}

bool AmdFx::FrcConfigure(UINT w, UINT h, std::string& err) {
  if (!ready_) {
    err = status;
    return false;
  }
  if (p_->frc && p_->frcW == w && p_->frcH == h) return true;
  FrcReset();
  amf::AMFComponentPtr c;
  AMF_RESULT r = p_->factory->CreateComponent(p_->ctx, AMFFRC, &c);
  if (r != AMF_OK || !c) {
    err = "This driver has no AMD frame generation component (" + AmfErr(r) + "). Update AMD Adrenalin.";
    return false;
  }
  c->SetProperty(AMF_FRC_ENGINE_TYPE, (amf_int64)FRC_ENGINE_DX11);
  c->SetProperty(AMF_FRC_MODE, (amf_int64)FRC_x2_PRESENT);
  c->SetProperty(AMF_FRC_ENABLE_FALLBACK, (amf_bool) false);
  c->SetProperty(AMF_FRC_INDICATOR, (amf_bool) false);
  c->SetProperty(AMF_FRC_PROFILE, (amf_int64)(h > 1080 ? FRC_PROFILE_SUPER : FRC_PROFILE_HIGH));
  c->SetProperty(AMF_FRC_MV_SEARCH_MODE, (amf_int64)FRC_MV_SEARCH_NATIVE);
  c->SetProperty(AMF_FRC_USE_FUTURE_FRAME, (amf_bool) false);  // lowest latency
  r = c->Init(amf::AMF_SURFACE_RGBA, (amf_int32)w, (amf_int32)h);
  if (r != AMF_OK) {
    err = "AMD frame generation failed to start at " + std::to_string(w) + "x" + std::to_string(h) + " (" + AmfErr(r) + ").";
    return false;
  }
  p_->frc = c;
  p_->frcW = w;
  p_->frcH = h;
  Log("AMF FRC initialised %ux%u", w, h);
  return true;
}

int AmdFx::FrcProcess(ID3D11Texture2D* in, ID3D11Texture2D* outs[2]) {
  if (!p_->frc) return -1;
  p_->held[0] = nullptr;
  p_->held[1] = nullptr;
  amf::AMFSurfacePtr s;
  AMF_RESULT r = p_->ctx->CreateSurfaceFromDX11Native(in, &s, nullptr);
  if (r != AMF_OK || !s) {
    Log("AMF FRC: wrap input failed %d", (int)r);
    return -1;
  }
  r = p_->frc->SubmitInput(s);
  if (r == AMF_INPUT_FULL) {
    Log("AMF FRC: input full, frame skipped");
    return 0;
  }
  if (r != AMF_OK && r != AMF_NEED_MORE_INPUT) {
    Log("AMF FRC: SubmitInput failed %d", (int)r);
    return -1;
  }
  int n = 0;
  do {
    amf::AMFDataPtr d;
    r = p_->frc->QueryOutput(&d);
    if ((r != AMF_OK && r != AMF_REPEAT) || !d) break;
    amf::AMFSurfacePtr os(d);
    if (!os) break;
    amf::AMFPlane* plane = os->GetPlane(amf::AMF_PLANE_PACKED);
    if (!plane) break;
    outs[n] = static_cast<ID3D11Texture2D*>(plane->GetNative());
    p_->held[n] = os;
    n++;
  } while (r == AMF_REPEAT && n < 2);
  return n;
}

ID3D11Texture2D* AmdFx::Scale(int slot, ID3D11Texture2D* in, UINT inW, UINT inH, UINT outW, UINT outH, int algo, float sharp) {
  if (!ready_ || slot < 0 || slot > 1 || !in || scaleFailed_) return nullptr;
  auto& s = p_->sc[slot];
  s.held = nullptr;
  if (!s.c || s.inW != inW || s.inH != inH || s.outW != outW || s.outH != outH || s.algo != algo) {
    if (s.c) s.c->Terminate();
    s.c = nullptr;
    amf::AMFComponentPtr c;
    AMF_RESULT r = p_->factory->CreateComponent(p_->ctx, AMFHQScaler, &c);
    if (r != AMF_OK || !c) {
      Log("AMF HQ scaler unavailable: %d", (int)r);
      scaleFailed_ = true;
      return nullptr;
    }
    c->SetProperty(AMF_HQ_SCALER_ENGINE_TYPE, (amf_int64)amf::AMF_MEMORY_DX11);
    c->SetProperty(AMF_HQ_SCALER_OUTPUT_SIZE, AMFConstructSize((amf_int32)outW, (amf_int32)outH));
    c->SetProperty(AMF_HQ_SCALER_ALGORITHM, (amf_int64)algo);
    c->SetProperty(AMF_HQ_SCALER_KEEP_ASPECT_RATIO, (amf_bool) false);
    c->SetProperty(AMF_HQ_SCALER_FILL, (amf_bool) false);
    c->SetProperty(AMF_HQ_SCALER_SHARPNESS, (amf_float)sharp);
    r = c->Init(amf::AMF_SURFACE_RGBA, (amf_int32)inW, (amf_int32)inH);
    if (r != AMF_OK) {
      Log("AMF HQ scaler init failed (%ux%u -> %ux%u, algo %d): %d", inW, inH, outW, outH, algo, (int)r);
      // VideoSR 1.1 is experimental; don't give up on 1.0 because 1.1 failed.
      if (algo != AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_1) scaleFailed_ = true;
      s.algo = algo;
      s.inW = inW, s.inH = inH, s.outW = outW, s.outH = outH;
      return nullptr;
    }
    s.c = c;
    s.inW = inW, s.inH = inH, s.outW = outW, s.outH = outH;
    s.algo = algo;
    s.sharp = sharp;
    Log("AMF HQ scaler %ux%u -> %ux%u algo %d", inW, inH, outW, outH, algo);
  } else if (s.sharp != sharp) {
    s.c->SetProperty(AMF_HQ_SCALER_SHARPNESS, (amf_float)sharp);
    s.sharp = sharp;
  }
  amf::AMFSurfacePtr surf;
  if (p_->ctx->CreateSurfaceFromDX11Native(in, &surf, nullptr) != AMF_OK || !surf) return nullptr;
  AMF_RESULT r = s.c->SubmitInput(surf);
  if (r != AMF_OK && r != AMF_NEED_MORE_INPUT && r != AMF_INPUT_FULL) return nullptr;
  for (int tries = 0; tries < 4; tries++) {
    amf::AMFDataPtr d;
    r = s.c->QueryOutput(&d);
    if (d) {
      amf::AMFSurfacePtr os(d);
      if (!os) return nullptr;
      amf::AMFPlane* plane = os->GetPlane(amf::AMF_PLANE_PACKED);
      if (!plane) return nullptr;
      s.held = os;
      return static_cast<ID3D11Texture2D*>(plane->GetNative());
    }
    if (r != AMF_OK && r != AMF_REPEAT) break;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// H.264 encoder

bool AmdFx::EncActive() const { return p_->enc != nullptr; }

void AmdFx::EncStop() {
  if (p_->conv) p_->conv->Terminate();
  if (p_->enc) p_->enc->Terminate();
  p_->conv = nullptr;
  p_->enc = nullptr;
  p_->pending.clear();
  p_->ptsQ.clear();
}

bool AmdFx::EncStart(UINT w, UINT h, UINT fpsN, UINT fpsD, UINT mbps, std::vector<uint8_t>& header, std::string& err) {
  EncStop();
  if (!ready_) {
    err = status;
    return false;
  }
  amf::AMFComponentPtr conv, enc;
  AMF_RESULT r = p_->factory->CreateComponent(p_->ctx, AMFVideoConverter, &conv);
  if (r != AMF_OK || !conv) {
    err = "AMD colour converter unavailable (" + AmfErr(r) + ")";
    return false;
  }
  conv->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, (amf_int64)amf::AMF_MEMORY_DX11);
  conv->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, (amf_int64)amf::AMF_SURFACE_NV12);
  conv->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, AMFConstructSize((amf_int32)w, (amf_int32)h));
  conv->SetProperty(AMF_VIDEO_CONVERTER_COLOR_PROFILE, (amf_int64)AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
  conv->SetProperty(AMF_VIDEO_CONVERTER_INPUT_COLOR_RANGE, (amf_int64)AMF_COLOR_RANGE_FULL);
  conv->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_COLOR_RANGE, (amf_int64)AMF_COLOR_RANGE_STUDIO);
  conv->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_COLOR_PRIMARIES, (amf_int64)AMF_COLOR_PRIMARIES_BT709);
  conv->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_TRANSFER_CHARACTERISTIC, (amf_int64)AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
  r = conv->Init(amf::AMF_SURFACE_RGBA, (amf_int32)w, (amf_int32)h);
  if (r != AMF_OK) {
    err = "AMD colour converter failed to start (" + AmfErr(r) + ")";
    return false;
  }

  r = p_->factory->CreateComponent(p_->ctx, AMFVideoEncoderVCE_AVC, &enc);
  if (r != AMF_OK || !enc) {
    err = "AMD H.264 encoder unavailable (" + AmfErr(r) + ")";
    conv->Terminate();
    return false;
  }
  double fps = double(fpsN) / std::max<UINT>(1, fpsD);
  amf_int64 bits = amf_int64(mbps) * 1000000;
  enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, (amf_int64)AMF_VIDEO_ENCODER_USAGE_TRANSCODING);  // must be first
  enc->SetProperty(AMF_VIDEO_ENCODER_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_PROFILE_HIGH);
  enc->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL, (amf_int64)AMF_H264_LEVEL__5_2);
  enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize((amf_int32)w, (amf_int32)h));
  enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fpsN, std::max<UINT>(1, fpsD)));
  enc->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)0);  // no B-frames: output order == input order
  enc->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, (amf_int64)AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY);
  enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, (amf_int64)AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
  enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bits);
  enc->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bits * 3 / 2);
  enc->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bits);
  amf_int64 gop = std::max<amf_int64>(1, amf_int64(fps * 2 + 0.5));  // keyframe every 2 s
  enc->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, gop);
  enc->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, gop);
  enc->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, (amf_int64)AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
  enc->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, (amf_int64)AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
  enc->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, (amf_int64)AMF_COLOR_PRIMARIES_BT709);
  r = enc->Init(amf::AMF_SURFACE_NV12, (amf_int32)w, (amf_int32)h);
  if (r != AMF_OK) {
    err = "AMD H.264 encoder rejected " + std::to_string(w) + "x" + std::to_string(h) + " (" + AmfErr(r) + ")";
    conv->Terminate();
    enc->Terminate();
    return false;
  }
  header.clear();
  amf::AMFVariant v;
  if (enc->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &v) == AMF_OK && v.type == amf::AMF_VARIANT_INTERFACE && v.pInterface) {
    amf::AMFInterfacePtr ip(v.pInterface);
    amf::AMFBufferPtr buf(ip);
    if (buf && buf->GetSize())
      header.assign((uint8_t*)buf->GetNative(), (uint8_t*)buf->GetNative() + buf->GetSize());
  }
  p_->conv = conv;
  p_->enc = enc;
  Log("AMF H.264 encoder %ux%u @ %.2f fps, %u Mbps, header %zu bytes", w, h, fps, mbps, header.size());
  return true;
}

static void CollectEncoded(amf::AMFComponent* enc, std::deque<int64_t>& ptsQ, std::vector<EncodedPacket>& out,
                           AMF_RESULT& last) {
  for (;;) {
    amf::AMFDataPtr d;
    last = enc->QueryOutput(&d);
    if (!d) return;
    amf::AMFBufferPtr b(d);
    if (!b) continue;
    EncodedPacket p;
    auto* bytes = (uint8_t*)b->GetNative();
    p.data.assign(bytes, bytes + b->GetSize());
    // Use our own timestamps rather than trusting the encoder to pass them through unchanged.
    if (!ptsQ.empty()) {
      p.pts = ptsQ.front();
      ptsQ.pop_front();
    } else {
      p.pts = d->GetPts();
    }
    amf_int64 type = -1;
    d->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
    p.key = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR || type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
    out.push_back(std::move(p));
  }
}

bool AmdFx::EncSubmit(ID3D11Texture2D* rgba, int64_t pts, int64_t dur) {
  if (!p_->enc || !p_->conv) return false;
  amf::AMFSurfacePtr in;
  if (p_->ctx->CreateSurfaceFromDX11Native(rgba, &in, nullptr) != AMF_OK || !in) return false;
  AMF_RESULT r = p_->conv->SubmitInput(in);
  if (r != AMF_OK) {
    Log("AMF converter SubmitInput: %d", (int)r);
    return false;
  }
  amf::AMFDataPtr nv12;
  for (int i = 0; i < 3 && !nv12; i++) p_->conv->QueryOutput(&nv12);
  if (!nv12) return true;  // skip this frame rather than fail the recording
  nv12->SetPts(pts);
  nv12->SetDuration(dur);
  for (int tries = 0; tries < 100; tries++) {
    r = p_->enc->SubmitInput(nv12);
    if (r != AMF_INPUT_FULL) break;
    AMF_RESULT q;
    CollectEncoded(p_->enc, p_->ptsQ, p_->pending, q);
    Sleep(1);
  }
  if (r == AMF_OK) p_->ptsQ.push_back(pts);
  else Log("AMF encoder SubmitInput: %d", (int)r);
  return r == AMF_OK || r == AMF_INPUT_FULL;
}

void AmdFx::EncPoll(std::vector<EncodedPacket>& out, bool drain) {
  if (!p_->enc) return;
  for (auto& p : p_->pending) out.push_back(std::move(p));
  p_->pending.clear();
  AMF_RESULT last = AMF_OK;
  if (!drain) {
    CollectEncoded(p_->enc, p_->ptsQ, out, last);
    return;
  }
  p_->enc->Drain();
  int64_t until = Now100ns() + 30000000;  // 3 s
  while (Now100ns() < until) {
    CollectEncoded(p_->enc, p_->ptsQ, out, last);
    if (last == AMF_EOF) break;
    Sleep(1);
  }
  if (last != AMF_EOF) Log("AMF encoder drain timed out");
}
