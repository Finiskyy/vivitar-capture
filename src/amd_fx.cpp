#include "amd_fx.h"

#include "components/FRC.h"
#include "components/HQScaler.h"
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
