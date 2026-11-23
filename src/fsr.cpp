#include "fsr.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cctype>

#include "../third_party/ffx/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/ffx/api/include/ffx_api.h"
#include "../third_party/ffx/upscalers/include/ffx_upscale.h"

namespace {
struct Shared {
  ComPtr<ID3D12Resource> r12;
  ComPtr<ID3D11Texture2D> t11;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
  UINT w = 0, h = 0;
};

void FfxMessage(uint32_t type, const wchar_t* msg) { Log("FSR [%u]: %s", type, W2U(msg ? msg : L"").c_str()); }
}  // namespace

struct FsrUpscaler::Impl {
  HMODULE loader = nullptr;
  PfnFfxCreateContext CreateContext = nullptr;
  PfnFfxDestroyContext DestroyContext = nullptr;
  PfnFfxConfigure Configure = nullptr;
  PfnFfxQuery Query = nullptr;
  PfnFfxDispatch Dispatch = nullptr;

  ComPtr<ID3D11Device1> dev11;
  ComPtr<ID3D11DeviceContext4> ctx11;
  ComPtr<ID3D11Device5> dev11_5;
  ComPtr<ID3D12Device> dev12;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence12;
  ComPtr<ID3D11Fence> fence11;
  uint64_t fv = 0;
  HANDLE evt = nullptr;

  std::vector<uint64_t> versionIds;
  std::vector<std::string> versionNames;

  struct Slot {
    ffxContext ctx = nullptr;
    int major = 0;
    UINT maxRW = 0, maxRH = 0, maxOW = 0, maxOH = 0;
    Shared color, mv, depth, out;
    ComPtr<ID3D12CommandAllocator> alloc[2];
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t allocFence[2] = {0, 0};
    int ai = 0;
    int64_t lastT = 0;
    std::string name;
  } slots[2];

  bool MakeShared(Shared& s, UINT w, UINT h, DXGI_FORMAT f, bool uav) {
    if (s.r12 && s.w == w && s.h == h) return true;
    s = Shared{};
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&s.r12));
    if (FAILED(hr)) {
      Log("FSR: CreateCommittedResource %ux%u fmt %d failed: %s", w, h, (int)f, HrStr(hr).c_str());
      return false;
    }
    HANDLE hnd = nullptr;
    hr = dev12->CreateSharedHandle(s.r12.Get(), nullptr, GENERIC_ALL, nullptr, &hnd);
    if (FAILED(hr)) {
      Log("FSR: CreateSharedHandle failed: %s", HrStr(hr).c_str());
      return false;
    }
    hr = dev11->OpenSharedResource1(hnd, IID_PPV_ARGS(&s.t11));
    CloseHandle(hnd);
    if (FAILED(hr)) {
      Log("FSR: OpenSharedResource1 failed: %s", HrStr(hr).c_str());
      return false;
    }
    dev11->CreateRenderTargetView(s.t11.Get(), nullptr, &s.rtv);
    dev11->CreateShaderResourceView(s.t11.Get(), nullptr, &s.srv);
    s.w = w;
    s.h = h;
    return true;
  }

  void DestroySlot(Slot& s) {
    if (s.ctx) {
      WaitIdle();
      DestroyContext(&s.ctx, nullptr);
    }
    s.ctx = nullptr;
    s.major = 0;
    s.maxRW = s.maxRH = s.maxOW = s.maxOH = 0;
  }

  void WaitIdle() {
    if (!queue) return;
    queue->Signal(fence12.Get(), ++fv);
    if (fence12->GetCompletedValue() < fv) {
      fence12->SetEventOnCompletion(fv, evt);
      WaitForSingleObject(evt, 2000);
    }
  }

  int64_t FindVersion(int major) const {
    // Highest version whose name contains "<major>." (names look like "FSR 4.1.2" / "3.1.5").
    int64_t best = -1;
    for (size_t i = 0; i < versionNames.size(); i++) {
      const std::string& n = versionNames[i];
      for (size_t k = 0; k + 1 < n.size(); k++) {
        if (n[k] == char('0' + major) && n[k + 1] == '.' && (k == 0 || !isdigit((unsigned char)n[k - 1]))) {
          best = (int64_t)i;  // SDK lists newest first, but keep scanning to prefer the first match
          return best;
        }
      }
    }
    return best;
  }
};

FsrUpscaler::FsrUpscaler() : p_(std::make_unique<Impl>()) {}

FsrUpscaler::~FsrUpscaler() {
  if (!p_) return;
  for (auto& s : p_->slots) p_->DestroySlot(s);
  p_->WaitIdle();
  if (p_->evt) CloseHandle(p_->evt);
}

bool FsrUpscaler::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  // The SDK DLLs ship next to the exe.
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
  p_->loader = LoadLibraryW((dir + L"amd_fidelityfx_loader_dx12.dll").c_str());
  if (!p_->loader) {
    status = "FSR files not found. Download the full zip (VivitarCapture.zip) so the FSR DLLs sit next to the app.";
    Log("FSR: amd_fidelityfx_loader_dx12.dll not found next to the exe");
    return false;
  }
  p_->CreateContext = (PfnFfxCreateContext)GetProcAddress(p_->loader, "ffxCreateContext");
  p_->DestroyContext = (PfnFfxDestroyContext)GetProcAddress(p_->loader, "ffxDestroyContext");
  p_->Configure = (PfnFfxConfigure)GetProcAddress(p_->loader, "ffxConfigure");
  p_->Query = (PfnFfxQuery)GetProcAddress(p_->loader, "ffxQuery");
  p_->Dispatch = (PfnFfxDispatch)GetProcAddress(p_->loader, "ffxDispatch");
  if (!p_->CreateContext || !p_->DestroyContext || !p_->Query || !p_->Dispatch) {
    status = "The FSR loader DLL is damaged or the wrong version.";
    return false;
  }

  // Direct3D 11 interfaces needed for sharing.
  if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&p_->dev11))) || FAILED(dev->QueryInterface(IID_PPV_ARGS(&p_->dev11_5))) ||
      FAILED(ctx->QueryInterface(IID_PPV_ARGS(&p_->ctx11)))) {
    status = "Windows 10 or newer is needed for FSR (Direct3D 11.4).";
    return false;
  }

  // A Direct3D 12 device on the same GPU.
  ComPtr<IDXGIDevice> dxgiDev;
  ComPtr<IDXGIAdapter> ad;
  dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
  if (!dxgiDev || FAILED(dxgiDev->GetAdapter(&ad))) {
    status = "Couldn't find the graphics adapter.";
    return false;
  }
  HRESULT hr = D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&p_->dev12));
  if (FAILED(hr)) {
    status = "Couldn't create a Direct3D 12 device (" + HrStr(hr) + ").";
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  p_->dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&p_->queue));
  hr = p_->dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&p_->fence12));
  HANDLE fh = nullptr;
  if (SUCCEEDED(hr)) hr = p_->dev12->CreateSharedHandle(p_->fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fh);
  if (SUCCEEDED(hr)) hr = p_->dev11_5->OpenSharedFence(fh, IID_PPV_ARGS(&p_->fence11));
  if (fh) CloseHandle(fh);
  if (FAILED(hr) || !p_->queue) {
    status = "Couldn't link Direct3D 11 and 12 (" + HrStr(hr) + ").";
    return false;
  }
  p_->evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // Which FSR versions does this SDK/driver provide?
  uint64_t count = 0;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = p_->dev12.Get();
  q.outputCount = &count;
  p_->Query(nullptr, &q.header);
  if (count) {
    p_->versionIds.resize(count);
    std::vector<const char*> names(count);
    q.versionIds = p_->versionIds.data();
    q.versionNames = names.data();
    p_->Query(nullptr, &q.header);
    for (uint64_t i = 0; i < count; i++) {
      std::string n = names[i] ? names[i] : "?";
      p_->versionNames.push_back(n);
      versions.push_back(n);
      Log("FSR version available: %s", n.c_str());
    }
  }
  if (versions.empty()) {
    status = "No FSR upscalers available.";
    return false;
  }
  status = "FidelityFX SDK ready";
  ready_ = true;
  return true;
}

bool FsrUpscaler::HasMajor(int major) const { return ready_ && p_->FindVersion(major) >= 0; }

std::string FsrUpscaler::ActiveName(int slot) const { return (slot >= 0 && slot < 2) ? p_->slots[slot].name : ""; }

ID3D11ShaderResourceView* FsrUpscaler::Upscale(int slot, int major, ID3D11Texture2D* color, UINT rw, UINT rh,
                                               const std::function<void(ID3D11RenderTargetView*, UINT, UINT)>& drawMotion,
                                               UINT ow, UINT oh, float mvScale, float sharpness, bool reset) {
  if (!ready_ || slot < 0 || slot > 1 || !color || Failed(major)) return nullptr;
  auto& P = *p_;
  auto& s = P.slots[slot];

  // (Re)create the context when the version or the maximum sizes change.
  if (!s.ctx || s.major != major || rw > s.maxRW || rh > s.maxRH || ow > s.maxOW || oh > s.maxOH) {
    P.DestroySlot(s);
    int64_t vi = P.FindVersion(major);
    if (vi < 0) {
      (major == 4 ? failed4_ : failed3_) = true;
      return nullptr;
    }
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = P.dev12.Get();
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion ovr{};
    ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ovr.versionId = P.versionIds[(size_t)vi];
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    // Leave headroom so window resizes don't recreate the context.
    s.maxRW = std::max<UINT>(rw, 1920);
    s.maxRH = std::max<UINT>(rh, 1080);
    s.maxOW = std::max<UINT>(ow, ow <= 2560 && oh <= 1440 ? 2560 : 3840);
    s.maxOH = std::max<UINT>(oh, ow <= 2560 && oh <= 1440 ? 1440 : 2160);
    cd.maxRenderSize = {s.maxRW, s.maxRH};
    cd.maxUpscaleSize = {s.maxOW, s.maxOH};
    cd.fpMessage = FfxMessage;
    cd.header.pNext = &backend.header;
    backend.header.pNext = &ver.header;
    ver.header.pNext = &ovr.header;
    ffxReturnCode_t rc = P.CreateContext(&s.ctx, &cd.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !s.ctx) {
      Log("FSR %d: CreateContext failed (%u)", major, rc);
      s.ctx = nullptr;
      (major == 4 ? failed4_ : failed3_) = true;
      return nullptr;
    }
    s.major = major;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (P.Query(&s.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) s.name = pv.versionName;
    else s.name = P.versionNames[(size_t)vi];
    Log("FSR context created (slot %d): %s, render %ux%u, output up to %ux%u", slot, s.name.c_str(), s.maxRW, s.maxRH,
        s.maxOW, s.maxOH);
    for (auto& a : s.alloc) P.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    P.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc[0].Get(), nullptr, IID_PPV_ARGS(&s.list));
    s.list->Close();
    reset = true;
  }

  bool depthNew = !s.depth.r12 || s.depth.w != rw || s.depth.h != rh;
  bool outNew = !s.out.r12 || s.out.w != ow || s.out.h != oh;
  if (!P.MakeShared(s.color, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, false) ||
      !P.MakeShared(s.mv, rw, rh, DXGI_FORMAT_R16G16_FLOAT, false) ||
      !P.MakeShared(s.depth, rw, rh, DXGI_FORMAT_R32_FLOAT, false) ||
      !P.MakeShared(s.out, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, true)) {
    (major == 4 ? failed4_ : failed3_) = true;
    return nullptr;
  }
  if (outNew) reset = true;

  // --- Direct3D 11: fill the shared inputs.
  D3D11_TEXTURE2D_DESC cdsc;
  color->GetDesc(&cdsc);
  if ((cdsc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || cdsc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) && cdsc.Width == rw &&
      cdsc.Height == rh && cdsc.ArraySize == 1)
    P.ctx11->CopyResource(s.color.t11.Get(), color);
  else
    return nullptr;
  drawMotion(s.mv.rtv.Get(), rw, rh);
  if (depthNew) {
    float one[4] = {1, 0, 0, 0};  // constant depth: video has none
    P.ctx11->ClearRenderTargetView(s.depth.rtv.Get(), one);
  }
  P.ctx11->Signal(P.fence11.Get(), ++P.fv);
  P.ctx11->Flush();

  // --- Direct3D 12: run FSR.
  P.queue->Wait(P.fence12.Get(), P.fv);
  if (P.fence12->GetCompletedValue() < s.allocFence[s.ai]) {
    P.fence12->SetEventOnCompletion(s.allocFence[s.ai], P.evt);
    WaitForSingleObject(P.evt, 1000);
  }
  s.alloc[s.ai]->Reset();
  s.list->Reset(s.alloc[s.ai].Get(), nullptr);
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
  };
  D3D12_RESOURCE_BARRIER pre[4] = {
      barrier(s.color.r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      barrier(s.mv.r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      barrier(s.depth.r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      barrier(s.out.r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
  s.list->ResourceBarrier(4, pre);

  int64_t now = Now100ns();
  float dtMs = s.lastT ? float(double(now - s.lastT) / 10000.0) : 16.6f;
  s.lastT = now;
  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = s.list.Get();
  d.color = ffxApiGetResourceDX12(s.color.r12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = ffxApiGetResourceDX12(s.depth.r12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = ffxApiGetResourceDX12(s.mv.r12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = ffxApiGetResourceDX12(s.out.r12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {0.f, 0.f};  // video has no camera jitter
  d.motionVectorScale = {mvScale, mvScale};
  d.renderSize = {rw, rh};
  d.upscaleSize = {ow, oh};
  d.enableSharpening = sharpness > 0.f;
  d.sharpness = std::clamp(sharpness, 0.f, 1.f);
  d.frameTimeDelta = std::clamp(dtMs, 1.f, 100.f);
  d.preExposure = 1.f;
  d.reset = reset;
  d.cameraNear = 0.1f;
  d.cameraFar = 1000.f;
  d.cameraFovAngleVertical = 1.0f;
  d.viewSpaceToMetersFactor = 1.f;
  d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  ffxReturnCode_t rc = P.Dispatch(&s.ctx, &d.header);

  D3D12_RESOURCE_BARRIER post[4] = {
      barrier(s.color.r12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
      barrier(s.mv.r12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
      barrier(s.depth.r12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
      barrier(s.out.r12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)};
  s.list->ResourceBarrier(4, post);
  s.list->Close();
  ID3D12CommandList* lists[] = {s.list.Get()};
  P.queue->ExecuteCommandLists(1, lists);
  P.queue->Signal(P.fence12.Get(), ++P.fv);
  s.allocFence[s.ai] = P.fv;
  s.ai ^= 1;

  // --- Direct3D 11 waits for FSR before using the output.
  P.ctx11->Wait(P.fence11.Get(), P.fv);
  if (rc != FFX_API_RETURN_OK) {
    static int logged = 0;
    if (logged++ < 5) Log("FSR %d: Dispatch failed (%u)", major, rc);
    return nullptr;
  }
  return s.out.srv.Get();
}
