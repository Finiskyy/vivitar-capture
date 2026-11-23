// Vivitar Capture — native Windows capture app (Media Foundation + Direct3D 11 + Dear ImGui).
#include "audio_out.h"
#include "capture.h"
#include "common.h"
#include "recorder.h"
#include "renderer.h"
#include "settings.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------------------------------------------
// Globals: window + D3D

static HWND g_hwnd = nullptr;
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<IDXGISwapChain1> g_swap;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static HANDLE g_frameWait = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static std::string g_gpuName = "-";
static const UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

static void CreateRTV() {
  ComPtr<ID3D11Texture2D> bb;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
  g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

static bool CreateDeviceD3D(HWND hwnd) {
  ComPtr<IDXGIFactory2> f2;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f2)))) return false;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIFactory6> f6;
  if (SUCCEEDED(f2.As(&f6))) f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL got;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, levels, 2, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
  if (FAILED(hr)) {
    adapter.Reset();
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &g_dev, &got,
                           &g_ctx);
  }
  if (FAILED(hr)) {
    Log("D3D11CreateDevice failed: %s", HrStr(hr).c_str());
    return false;
  }
  ComPtr<IDXGIDevice> dxgiDev;
  ComPtr<IDXGIAdapter> ad;
  if (SUCCEEDED(g_dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
    DXGI_ADAPTER_DESC d;
    ad->GetDesc(&d);
    g_gpuName = W2U(d.Description);
  }
  Log("GPU: %s", g_gpuName.c_str());

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = kSwapFlags;
  hr = f2->CreateSwapChainForHwnd(g_dev.Get(), hwnd, &sd, nullptr, nullptr, &g_swap);
  if (FAILED(hr)) {
    Log("CreateSwapChainForHwnd failed: %s", HrStr(hr).c_str());
    return false;
  }
  f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  ComPtr<IDXGISwapChain2> sc2;
  if (SUCCEEDED(g_swap.As(&sc2))) {
    sc2->SetMaximumFrameLatency(1);
    g_frameWait = sc2->GetFrameLatencyWaitableObject();
  }
  CreateRTV();
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Small helpers

static ImU32 Col(uint32_t rgb, float a = 1.f) {
  return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, int(a * 255));
}
static ImVec4 V4(uint32_t rgb, float a = 1.f) {
  return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
static const uint32_t cBg = 0x0f1114, cPanel = 0x181b20, cPanel2 = 0x1f232a, cLine = 0x2a2f37, cLine2 = 0x353b45,
                      cText = 0xe7e9ec, cMuted = 0x8b929d, cDim = 0x5d636d, cAccent = 0x1f8fff, cAccent2 = 0x4aa5ff,
                      cRec = 0xff3b3b, cOk = 0x2ecc71, cWarn = 0xf5a623;

static std::string FmtTime(int64_t t100) {
  int64_t s = t100 / 10000000;
  char b[32];
  snprintf(b, sizeof(b), "%02lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
  return b;
}
static std::string FmtSize(uint64_t b) {
  char o[32];
  if (b < 1024ull * 1024) snprintf(o, sizeof(o), "%.0f KB", b / 1024.0);
  else if (b < 1024ull * 1024 * 1024) snprintf(o, sizeof(o), "%.1f MB", b / 1048576.0);
  else snprintf(o, sizeof(o), "%.2f GB", b / 1073741824.0);
  return o;
}

static bool SavePng(const std::wstring& path, const uint8_t* rgba, UINT w, UINT h) {
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  std::vector<uint8_t> bgr(size_t(w) * h * 3);
  for (size_t i = 0, n = size_t(w) * h; i < n; i++) {
    bgr[i * 3] = rgba[i * 4 + 2];
    bgr[i * 3 + 1] = rgba[i * 4 + 1];
    bgr[i * 3 + 2] = rgba[i * 4];
  }
  ComPtr<IWICStream> s;
  ComPtr<IWICBitmapEncoder> e;
  ComPtr<IWICBitmapFrameEncode> fr;
  ComPtr<IPropertyBag2> pb;
  if (FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) || FAILED(e->Initialize(s.Get(), WICBitmapEncoderNoCache)))
    return false;
  if (FAILED(e->CreateNewFrame(&fr, &pb)) || FAILED(fr->Initialize(pb.Get()))) return false;
  fr->SetSize(w, h);
  WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
  fr->SetPixelFormat(&pf);
  if (pf != GUID_WICPixelFormat24bppBGR) return false;
  if (FAILED(fr->WritePixels(h, w * 3, (UINT)bgr.size(), bgr.data()))) return false;
  return SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
}

static bool IsCaptureName(const std::wstring& n) {
  static const wchar_t* yes[] = {L"vivitar", L"capture", L"usb video", L"hdmi", L"ms21", L"macrosilicon", L"video grab",
                                 L"uvc", L"cam link", L"easycap", L"game", L"usb3.0 video", L"usb2.0 pc"};
  for (auto k : yes)
    if (ContainsI(n, k)) return true;
  return false;
}
static bool IsWebcamName(const std::wstring& n) {
  static const wchar_t* no[] = {L"integrated", L"webcam", L"front", L"rear", L"ir camera", L"virtual", L"obs"};
  for (auto k : no)
    if (ContainsI(n, k)) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------
// App

struct Toast {
  std::string text;
  uint32_t color;
  double until;
};

struct LibItem {
  std::wstring path, name;
  uint64_t size = 0;
  FILETIME mtime{};
  bool video = false;
};

static double NowSecs() { return double(Now100ns()) / 1e7; }

class App {
 public:
  float s = 1.f;  // DPI scale
  ImFont* fBody = nullptr;
  ImFont* fBold = nullptr;
  ImFont* fBig = nullptr;
  bool quit = false;

  void Init();
  void Shutdown();
  void Tick();
  void UI();
  void RenderPreview();
  void OnClose() {
    if (recording) StopRecording();
  }

 private:
  // devices
  void Scan();
  void OpenDevice();
  std::wstring PickVideo();
  std::wstring PickAudio(const std::wstring& videoName);
  // actions
  void StartRecording();
  void StopRecording();
  void Screenshot();
  void ToggleFullscreen();
  void ChooseFolder();
  void ScanLibrary();
  void ToastMsg(const std::string& t, uint32_t c = cText, double secs = 3.5);
  // params
  ColorParams Color() const;
  EnhanceParams Enh() const;
  void OutSize(UINT& w, UINT& h) const;
  void ApplyPreset(int p);
  void Dirty() {
    settingsDirty = true;
    lastChange = NowSecs();
  }
  void UpdateMonitorVolume();
  // ui pieces
  void TopBar(float h);
  void Stage();
  void Sidebar();
  void BottomBar(float h);
  void Library();
  void DrawToasts();
  bool Seg(const char* id, int* v, std::initializer_list<const char*> items);
  bool Toggle(const char* id, const char* label, bool* v);
  bool LabeledSlider(const char* id, const char* label, int* v, int lo, int hi, const char* fmt);
  void Section(const char* title);

  Settings S;
  bool settingsDirty = false;
  double lastSave = 0, lastChange = 0;

  struct OpenResult {
    std::unique_ptr<VideoCapture> cap;
    std::unique_ptr<AudioCapture> acap;
    std::string capErr, audErr;
  };
  std::unique_ptr<VideoCapture> cap = std::make_unique<VideoCapture>();
  std::unique_ptr<AudioCapture> acap = std::make_unique<AudioCapture>();
  std::future<OpenResult> opening;
  bool reopenQueued = false;
  void FinishOpen(OpenResult r);
  AudioMonitor mon;
  Recorder rec;
  Renderer R;
  std::vector<DeviceInfo> vids, auds;
  std::wstring curVideoName;
  std::string capErr, audErr;
  double nextScan = 0;
  bool everOpened = false;

  RenderTarget preview, recRT, shotRT;
  int wantPW = 0, wantPH = 0;
  float splitFrac = 0.5f;
  bool compare = false;
  std::vector<VideoFrame> frames;
  int frameCount = 0;
  double fpsT = 0, measuredFps = 0;

  bool recording = false, recEnh = false;
  int64_t recStart = 0;
  uint64_t recBytes = 0;
  double recBytesT = 0;

  std::mutex toastM;
  std::vector<Toast> toasts;
  double flashUntil = 0;

  int tab = 0;
  std::vector<LibItem> lib;
  bool libDirty = true;
  int pendingDelete = -1;

  bool fullscreen = false;
  WINDOWPLACEMENT wpPrev{sizeof(WINDOWPLACEMENT)};
  double fsHintUntil = 0;
  float meterL = 0, meterR = 0, peakHoldL = 0, peakHoldR = 0;
  double peakTL = 0, peakTR = 0;
};

static double NowSec() { return double(Now100ns()) / 1e7; }

void App::Init() {
  S.Load();
  std::string err;
  if (!R.Init(g_dev.Get(), g_ctx.Get(), err)) ToastMsg(err, cRec, 10);
  UpdateMonitorVolume();
  mon.Start();
  Scan();
  OpenDevice();
}

void App::Shutdown() {
  if (recording) StopRecording();
  if (opening.valid()) FinishOpen(opening.get());
  cap->Close();
  acap->Close();
  mon.Stop();
  S.Save();
}

void App::UpdateMonitorVolume() {
  float v = S.monitor && !S.muted ? std::pow(S.volume / 100.f, 2.f) * 1.5f : 0.f;
  mon.volume = v;
}

void App::Scan() {
  vids = EnumDevices(false);
  auds = EnumDevices(true);
  for (auto& d : vids) Log("Video device: %s", W2U(d.name).c_str());
  for (auto& d : auds) Log("Audio device: %s", W2U(d.name).c_str());
}

std::wstring App::PickVideo() {
  for (auto& d : vids)
    if (d.id == S.videoId) return d.id;
  for (auto& d : vids)
    if (IsCaptureName(d.name) && !IsWebcamName(d.name)) return d.id;
  for (auto& d : vids)
    if (!IsWebcamName(d.name)) return d.id;
  return vids.empty() ? L"" : vids[0].id;
}

std::wstring App::PickAudio(const std::wstring& videoName) {
  if (!S.audioAuto) {
    for (auto& a : auds)
      if (a.id == S.audioId) return a.id;
    return L"";
  }
  // Same name as the video device first (most cards expose "XYZ" for both), then anything capture-like.
  if (!videoName.empty())
    for (auto& a : auds)
      if (ContainsI(a.name, videoName.c_str()) || ContainsI(videoName, a.name.c_str())) return a.id;
  for (auto& a : auds)
    if (IsCaptureName(a.name) || ContainsI(a.name, L"digital audio interface") || ContainsI(a.name, L"usb digital audio"))
      return a.id;
  return L"";
}

// Closing and opening devices can take a second or more, so it happens on a worker thread.
void App::OpenDevice() {
  if (recording) StopRecording();
  if (opening.valid()) {
    reopenQueued = true;
    return;
  }
  capErr.clear();
  audErr.clear();
  std::wstring vid = PickVideo();
  curVideoName.clear();
  for (auto& d : vids)
    if (d.id == vid) curVideoName = d.name;
  std::wstring aud = PickAudio(curVideoName);
  CaptureRequest req;
  req.id = vid;
  req.w = S.width;
  req.h = S.height;
  req.fps = S.fps;
  req.fmtPref = S.fmtPref;
  auto oldCap = std::move(cap);
  auto oldAud = std::move(acap);
  cap = std::make_unique<VideoCapture>();
  acap = std::make_unique<AudioCapture>();
  opening = std::async(std::launch::async, [this, req, aud, oldCap = std::move(oldCap), oldAud = std::move(oldAud)]() mutable {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    oldAud.reset();  // destructors close the devices
    oldCap.reset();
    OpenResult r;
    r.cap = std::make_unique<VideoCapture>();
    r.acap = std::make_unique<AudioCapture>();
    r.acap->onAudio = [this](const float* d, size_t n, int64_t t) {
      mon.Push(d, n);
      rec.PushAudio(d, n, t);
    };
    std::string err;
    if (req.id.empty()) r.capErr = "nodevice";
    else if (!r.cap->Open(req, err)) r.capErr = err;
    else if (!aud.empty() && !r.acap->Open(aud, err)) r.audErr = err;
    CoUninitialize();
    return r;
  });
}

void App::FinishOpen(OpenResult r) {
  cap = std::move(r.cap);
  acap = std::move(r.acap);
  capErr = r.capErr;
  audErr = r.audErr;
  if (!audErr.empty()) ToastMsg(audErr, cWarn, 6);
  if (cap->IsOpen()) everOpened = true;
  measuredFps = 0;
}

ColorParams App::Color() const {
  ColorParams c;
  if (S.range == 1) c.fullRange = false;
  else if (S.range == 2) c.fullRange = true;
  else c.fullRange = cap->nominalRange == 1;
  c.bt709 = R.srcH == 0 || R.srcH >= 720;
  return c;
}

EnhanceParams App::Enh() const {
  EnhanceParams e;
  if (!S.enh) return e;
  e.scaler = S.scaler;
  e.sharpen = S.sharpen / 100.f;
  e.denoise = S.denoise / 100.f;
  e.bri = S.bri / 250.f;
  e.con = S.con / 100.f;
  e.sat = S.sat / 100.f;
  return e;
}

void App::OutSize(UINT& w, UINT& h) const {
  w = R.srcW;
  h = R.srcH;
  if (S.outRes == 1 && h && h < 1440) {
    w = UINT(std::lround(double(w) * 1440.0 / h / 2.0) * 2);
    h = 1440;
  }
}

void App::ApplyPreset(int p) {
  static const int P[4][3] = {{1, 40, 0}, {1, 30, 15}, {2, 50, 30}, {2, 80, 55}};
  S.preset = p;
  if (p < 4) {
    S.scaler = P[p][0];
    S.sharpen = P[p][1];
    S.denoise = P[p][2];
  }
  Dirty();
}

void App::ToastMsg(const std::string& t, uint32_t c, double secs) {
  std::lock_guard<std::mutex> lk(toastM);
  toasts.push_back({t, c, NowSec() + secs});
  Log("Toast: %s", t.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// Per-frame work

void App::Tick() {
  double now = NowSec();

  // Background open finished?
  if (opening.valid() && opening.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    FinishOpen(opening.get());
    if (reopenQueued) {
      reopenQueued = false;
      OpenDevice();
    }
  }
  // Device loss / reconnect
  if (!opening.valid() && cap->IsOpen() && cap->Lost()) {
    if (recording) StopRecording();
    capErr = "lost";
    // Close the old devices off the UI thread (their destructors can wait on the driver).
    std::thread([c = std::move(cap), a = std::move(acap)]() mutable {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      a.reset();
      c.reset();
      CoUninitialize();
    }).detach();
    cap = std::make_unique<VideoCapture>();
    acap = std::make_unique<AudioCapture>();
    ToastMsg("Capture card disconnected", cWarn);
    nextScan = now + 1;
  }
  if (!opening.valid() && acap->IsOpen() && acap->Lost()) {
    std::thread([a = std::move(acap)]() mutable {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      a.reset();
      CoUninitialize();
    }).detach();
    acap = std::make_unique<AudioCapture>();
  }
  if (!opening.valid() && !cap->IsOpen() && now > nextScan && (capErr == "nodevice" || capErr == "lost")) {
    nextScan = now + 2;
    Scan();
    if (!PickVideo().empty()) OpenDevice();
  }

  // Frames
  frames.clear();
  cap->TakeFrames(frames);
  ColorParams color = Color();
  EnhanceParams enh = Enh();
  if (!frames.empty()) {
    frameCount += (int)frames.size();
    if (recording && recEnh) {
      UINT rw, rh;
      for (auto& f : frames) {
        R.Upload(f);
        R.Convert(color, enh.denoise);
        OutSize(rw, rh);
        R.Ensure(recRT, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM);
        R.Draw(recRT, FitContain, enh, color, -1.f, 1.f);
        R.SubmitNV12(recRT, f.t);
        R.CollectNV12([this](const uint8_t* d, size_t n, int64_t t) { rec.PushVideo(d, n, t); }, false);
      }
    } else {
      R.Upload(frames.back());
    }
    cap->Recycle(frames);
  }
  if (R.HasFrame()) R.Convert(color, enh.denoise);

  if (now - fpsT >= 1.0) {
    measuredFps = frameCount / (now - fpsT);
    frameCount = 0;
    fpsT = now;
  }
  if (recording && now - recBytesT > 0.5) {
    recBytes = rec.Bytes();
    recBytesT = now;
  }

  // Meters (-60..0 dBFS)
  auto pct = [](float v) {
    float db = 20.f * std::log10(std::max(v, 1e-6f));
    return std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
  };
  float l = pct(acap->peakL.exchange(0)), r = pct(acap->peakR.exchange(0));
  float dt = ImGui::GetIO().DeltaTime;
  meterL = std::max(l, meterL - dt * 1.2f);
  meterR = std::max(r, meterR - dt * 1.2f);
  if (meterL >= peakHoldL || now - peakTL > 1.2) peakHoldL = meterL, peakTL = now;
  if (meterR >= peakHoldR || now - peakTR > 1.2) peakHoldR = meterR, peakTR = now;

  if (settingsDirty && now - lastChange > 0.75) {
    S.Save();
    settingsDirty = false;
    lastSave = now;
  }
}

void App::RenderPreview() {
  if (wantPW <= 0 || wantPH <= 0) return;
  R.Ensure(preview, wantPW, wantPH, DXGI_FORMAT_R8G8B8A8_UNORM);
  float split = (compare && S.enh) ? splitFrac * preview.w : -1.f;
  R.Draw(preview, S.fit, Enh(), Color(), split, std::max(1.f, s));
}

// ---------------------------------------------------------------------------------------------------------------
// Actions

void App::StartRecording() {
  if (recording || !cap->IsOpen() || !R.HasFrame()) return;
  SHCreateDirectoryExW(nullptr, S.saveDir.c_str(), nullptr);
  recEnh = S.enh && S.enhRecord;
  RecordConfig cfg;
  if (recEnh) OutSize(cfg.w, cfg.h);
  else {
    cfg.w = cap->outW;
    cfg.h = cap->outH;
  }
  cfg.fpsN = cap->cur.fpsN ? cap->cur.fpsN : 60;
  cfg.fpsD = cap->cur.fpsD ? cap->cur.fpsD : 1;
  cfg.mbps = S.bitrate;
  cfg.audio = acap->IsOpen();
  cfg.path = S.saveDir + L"\\Vivitar Capture " + TimestampName() + L".mp4";
  recStart = Now100ns();
  std::string err;
  if (!rec.Start(cfg, recStart, err)) {
    ToastMsg(err, cRec, 8);
    return;
  }
  recording = true;
  recBytes = 0;
  if (recEnh) R.ResetNV12();
  else cap->SetOnFrame([this](const VideoFrame& f) { rec.PushVideo(f.data.data(), f.data.size(), f.t); });
  ToastMsg("Recording started");
}

void App::StopRecording() {
  if (!recording) return;
  cap->SetOnFrame(nullptr);
  if (recEnh) R.CollectNV12([this](const uint8_t* d, size_t n, int64_t t) { rec.PushVideo(d, n, t); }, true);
  recording = false;
  std::wstring path = rec.cfg.path;
  uint32_t dropped = rec.dropped.load();
  rec.Stop();
  std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
  ToastMsg("Saved \"" + W2U(name) + "\"" + (dropped ? "  (" + std::to_string(dropped) + " frames dropped)" : ""), cOk, 5);
  libDirty = true;
}

void App::Screenshot() {
  if (!R.HasFrame()) return;
  bool e = S.enh && S.enhRecord;
  UINT w = R.srcW, h = R.srcH;
  if (e) OutSize(w, h);
  R.Ensure(shotRT, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
  R.Draw(shotRT, FitContain, e ? Enh() : EnhanceParams{}, Color(), -1.f, 1.f);
  std::vector<uint8_t> px;
  if (!R.ReadRGBA(shotRT, px)) {
    ToastMsg("Screenshot failed", cRec);
    return;
  }
  SHCreateDirectoryExW(nullptr, S.saveDir.c_str(), nullptr);
  std::wstring path = S.saveDir + L"\\Vivitar Screenshot " + TimestampName() + L".png";
  flashUntil = NowSec() + 0.25;
  std::thread([this, px = std::move(px), w, h, path] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = SavePng(path, px.data(), w, h);
    CoUninitialize();
    ToastMsg(ok ? "Screenshot saved" : "Couldn't save the screenshot", ok ? cOk : cRec);
    libDirty = true;
  }).detach();
}

void App::ToggleFullscreen() {
  if (!fullscreen) {
    GetWindowPlacement(g_hwnd, &wpPrev);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    fsHintUntil = NowSec() + 2.5;
  } else {
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPlacement(g_hwnd, &wpPrev);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  fullscreen = !fullscreen;
}

void App::ChooseFolder() {
  ComPtr<IFileOpenDialog> d;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
  DWORD opts = 0;
  d->GetOptions(&opts);
  d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->SetTitle(L"Choose where to save captures");
  ComPtr<IShellItem> cur;
  if (SUCCEEDED(SHCreateItemFromParsingName(S.saveDir.c_str(), nullptr, IID_PPV_ARGS(&cur)))) d->SetFolder(cur.Get());
  if (FAILED(d->Show(g_hwnd))) return;
  ComPtr<IShellItem> it;
  PWSTR p = nullptr;
  if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
    S.saveDir = p;
    CoTaskMemFree(p);
    Dirty();
    libDirty = true;
    ToastMsg("Captures will save to " + W2U(S.saveDir), cOk);
  }
}

void App::ScanLibrary() {
  lib.clear();
  for (const wchar_t* pat : {L"\\*.mp4", L"\\*.png"}) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((S.saveDir + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      LibItem it;
      it.name = fd.cFileName;
      it.path = S.saveDir + L"\\" + fd.cFileName;
      it.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
      it.mtime = fd.ftLastWriteTime;
      it.video = ContainsI(it.name, L".mp4");
      lib.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(lib.begin(), lib.end(), [](const LibItem& a, const LibItem& b) { return CompareFileTime(&a.mtime, &b.mtime) > 0; });
}

// ---------------------------------------------------------------------------------------------------------------
// UI widgets

bool App::Seg(const char* id, int* v, std::initializer_list<const char*> items) {
  ImGui::PushID(id);
  float avail = ImGui::GetContentRegionAvail().x;
  int n = (int)items.size();
  float gap = 2 * s;
  float bw = (avail - gap * (n - 1)) / n;
  bool changed = false;
  int i = 0;
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5 * s);
  for (const char* it : items) {
    if (i) ImGui::SameLine(0, gap);
    bool on = *v == i;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? V4(cLine2) : V4(cPanel2));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? V4(cLine2) : V4(cLine));
    ImGui::PushStyleColor(ImGuiCol_Text, on ? V4(cText) : V4(cMuted));
    if (ImGui::Button(it, ImVec2(bw, 28 * s))) {
      *v = i;
      changed = true;
    }
    ImGui::PopStyleColor(3);
    i++;
  }
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}

bool App::Toggle(const char* id, const char* label, bool* v) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x, h = 24 * s;
  bool changed = false;
  if (ImGui::InvisibleButton(id, ImVec2(w, h))) {
    *v = !*v;
    changed = true;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(p.x, p.y + (h - ImGui::GetFontSize()) / 2), Col(cText), label);
  float tw = 36 * s, th = 20 * s;
  ImVec2 a(p.x + w - tw, p.y + (h - th) / 2), b(a.x + tw, a.y + th);
  dl->AddRectFilled(a, b, *v ? Col(cAccent) : Col(cLine2), th / 2);
  float kx = *v ? b.x - th / 2 : a.x + th / 2;
  dl->AddCircleFilled(ImVec2(kx, a.y + th / 2), th / 2 - 2.5f * s, *v ? Col(0xffffff) : Col(0xcfd3d8));
  return changed;
}

bool App::LabeledSlider(const char* id, const char* label, int* v, int lo, int hi, const char* fmt) {
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::TextColored(V4(cMuted), "%s", label);
  char b[32];
  snprintf(b, sizeof(b), fmt, *v);
  ImGui::SameLine(w - ImGui::CalcTextSize(b).x);
  ImGui::TextUnformatted(b);
  ImGui::SetNextItemWidth(-1);
  return ImGui::SliderInt(id, v, lo, hi, "");
}

void App::Section(const char* title) {
  ImGui::Dummy(ImVec2(0, 4 * s));
  ImGui::PushFont(fBold);
  ImGui::PushStyleColor(ImGuiCol_Text, V4(cMuted));
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 2 * s));
}

static void Divider(float s) {
  ImGui::Dummy(ImVec2(0, 6 * s));
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - 16 * s, p.y), ImVec2(p.x + w + 16 * s, p.y), Col(cLine), 1);
  ImGui::Dummy(ImVec2(0, 8 * s));
}

static void Muted(const char* t) {
  ImGui::PushStyleColor(ImGuiCol_Text, V4(cMuted));
  ImGui::TextWrapped("%s", t);
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------------------------
// UI layout

void App::UI() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V4(cBg));
  ImGui::Begin("root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);

  float W = vp->Size.x, H = vp->Size.y;
  if (fullscreen) {
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("stagefs", ImVec2(W, H), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    Stage();
    ImGui::EndChild();
  } else {
    float topH = 48 * s, botH = 84 * s, sideW = S.sidebar ? 330 * s : 0;
    ImGui::SetCursorPos(ImVec2(0, 0));
    TopBar(topH);
    ImGui::SetCursorPos(ImVec2(0, topH));
    ImGui::BeginChild("main", ImVec2(W - sideW, H - topH - botH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (tab == 0) Stage();
    else Library();
    ImGui::EndChild();
    if (S.sidebar) {
      ImGui::SetCursorPos(ImVec2(W - sideW, topH));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(cPanel));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 10 * s));
      ImGui::BeginChild("side", ImVec2(sideW, H - topH - botH), ImGuiChildFlags_AlwaysUseWindowPadding);
      Sidebar();
      ImGui::EndChild();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      ImVec2 a = ImGui::GetWindowPos();
      ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + W - sideW, a.y + topH), ImVec2(a.x + W - sideW, a.y + H - botH), Col(cLine));
    }
    ImGui::SetCursorPos(ImVec2(0, H - botH));
    BottomBar(botH);
  }
  DrawToasts();
  ImGui::End();

  // Keyboard shortcuts
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) recording ? StopRecording() : StartRecording();
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) Screenshot();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_F11, false)) ToggleFullscreen();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && fullscreen) ToggleFullscreen();
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
      if (!S.monitor) S.monitor = true, S.muted = false;
      else S.muted = !S.muted;
      UpdateMonitorVolume();
      Dirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && !recording) {
      S.enh = !S.enh;
      Dirty();
      ToastMsg(S.enh ? "Enhancement on" : "Enhancement off");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) && S.enh) compare = !compare;
  }
}

void App::TopBar(float h) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetWindowSize().x;
  dl->AddRectFilled(o, ImVec2(o.x + W, o.y + h), Col(cPanel));
  dl->AddLine(ImVec2(o.x, o.y + h - 1), ImVec2(o.x + W, o.y + h - 1), Col(cLine));
  // logo
  float ls = 24 * s;
  ImVec2 lp(o.x + 14 * s, o.y + (h - ls) / 2);
  dl->AddRectFilled(lp, ImVec2(lp.x + ls, lp.y + ls), Col(cAccent), 6 * s);
  ImVec2 c(lp.x + ls / 2, lp.y + ls / 2);
  dl->AddCircleFilled(c, 6 * s, Col(0xffffff));
  dl->AddCircleFilled(c, 3 * s, Col(cRec));
  ImGui::PushFont(fBold);
  dl->AddText(ImVec2(lp.x + ls + 9 * s, o.y + (h - ImGui::GetFontSize()) / 2), Col(cText), "VIVITAR CAPTURE");
  float x = lp.x + ls + 9 * s + ImGui::CalcTextSize("VIVITAR CAPTURE").x + 26 * s;
  // tabs
  const char* tabs[2] = {"Capture", "Library"};
  for (int i = 0; i < 2; i++) {
    ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
    float tw = ts.x + 32 * s;
    ImGui::SetCursorScreenPos(ImVec2(x, o.y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("tab", ImVec2(tw, h))) {
      tab = i;
      if (i == 1) libDirty = true;
    }
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    dl->AddText(ImVec2(x + 16 * s, o.y + (h - ts.y) / 2), tab == i ? Col(cText) : hov ? Col(cText) : Col(cMuted), tabs[i]);
    if (tab == i) dl->AddRectFilled(ImVec2(x, o.y + h - 3 * s), ImVec2(x + tw, o.y + h - 1), Col(cAccent));
    x += tw;
  }
  ImGui::PopFont();

  // status pill
  std::string st;
  uint32_t dot = cDim;
  if (recording) st = "Recording", dot = cRec;
  else if (cap->IsOpen() && R.HasFrame()) {
    char b[64];
    snprintf(b, sizeof(b), "Live  %ux%u  %.0f fps", R.srcW, R.srcH, measuredFps);
    st = b;
    dot = cOk;
  } else st = "No signal";
  ImVec2 ts = ImGui::CalcTextSize(st.c_str());
  float pw = ts.x + 34 * s, ph = 26 * s;
  float bx = o.x + W - 14 * s - 34 * s;
  ImVec2 pa(bx - 12 * s - pw, o.y + (h - ph) / 2);
  dl->AddRectFilled(pa, ImVec2(pa.x + pw, pa.y + ph), Col(cPanel2), ph / 2);
  dl->AddRect(pa, ImVec2(pa.x + pw, pa.y + ph), Col(cLine), ph / 2);
  float blink = recording ? (std::fmod(NowSec(), 1.0) < 0.5 ? 1.f : 0.3f) : 1.f;
  dl->AddCircleFilled(ImVec2(pa.x + 14 * s, pa.y + ph / 2), 4 * s, Col(dot, blink));
  dl->AddText(ImVec2(pa.x + 24 * s, pa.y + (ph - ts.y) / 2), recording || cap->IsOpen() ? Col(cText) : Col(cMuted), st.c_str());

  // sidebar toggle (gear-ish)
  ImGui::SetCursorScreenPos(ImVec2(bx, o.y + (h - 34 * s) / 2));
  if (ImGui::InvisibleButton("sidebtn", ImVec2(34 * s, 34 * s))) {
    S.sidebar = !S.sidebar;
    Dirty();
  }
  if (ImGui::IsItemHovered()) {
    dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Col(cPanel2), 8 * s);
    ImGui::SetTooltip("Settings");
  }
  ImVec2 gc((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
  uint32_t gcol = S.sidebar ? cAccent2 : cMuted;
  for (int i = 0; i < 3; i++) {
    float yy = gc.y - 5 * s + i * 5 * s;
    dl->AddLine(ImVec2(gc.x - 8 * s, yy), ImVec2(gc.x + 8 * s, yy), Col(gcol), 1.6f * s);
    float kx = gc.x + (i == 1 ? 3 : -3) * s;
    dl->AddCircleFilled(ImVec2(kx, yy), 2.6f * s, Col(gcol));
  }
}

void App::Stage() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
  float m = fullscreen ? 0 : 14 * s;
  ImVec2 a(std::floor(o.x + m), std::floor(o.y + m)), b(std::floor(o.x + sz.x - m), std::floor(o.y + sz.y - m));
  float w = b.x - a.x, h = b.y - a.y;
  wantPW = (int)w;
  wantPH = (int)h;
  float rnd = fullscreen ? 0 : 10 * s;
  dl->AddRectFilled(a, b, Col(0x000000), rnd);
  bool live = cap->IsOpen() && R.HasFrame();
  if (live && preview.srv) dl->AddImageRounded((ImTextureID)(uintptr_t)preview.srv.Get(), a, b, ImVec2(0, 0), ImVec2(1, 1), Col(0xffffff), rnd);
  if (!fullscreen) dl->AddRect(a, b, recording ? Col(cRec, 0.6f) : Col(cLine), rnd, 0, recording ? 1.5f * s : 1.f);

  // interaction: double-click fullscreen, drag compare split
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton("##stage", ImVec2(std::max(1.f, w), std::max(1.f, h)));
  bool comparing = compare && S.enh && live;
  if (comparing && ImGui::IsItemActive()) splitFrac = std::clamp((ImGui::GetIO().MousePos.x - a.x) / w, 0.f, 1.f);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) ToggleFullscreen();
  if (comparing && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

  ImGui::PushFont(fBold);
  auto badge = [&](ImVec2 p, const char* t, uint32_t col, bool right) {
    ImVec2 ts = ImGui::CalcTextSize(t);
    float bw = ts.x + 18 * s, bh = ts.y + 8 * s;
    ImVec2 q = right ? ImVec2(p.x - bw, p.y) : p;
    dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + bh), Col(0x000000, 0.6f), 6 * s);
    dl->AddText(ImVec2(q.x + 9 * s, q.y + 4 * s), Col(col), t);
    return bw;
  };
  if (live && !fullscreen) {
    char b2[64];
    snprintf(b2, sizeof(b2), "%up%.0f  %ux%u%s", R.srcH, measuredFps, R.srcW, R.srcH, S.enh ? "  ENHANCED" : "");
    badge(ImVec2(b.x - 12 * s, a.y + 12 * s), b2, S.enh ? cAccent2 : 0xcfd3d8, true);
  }
  if (recording) {
    std::string t = "  REC " + FmtTime(Now100ns() - recStart);
    float bw = badge(ImVec2(a.x + 12 * s, a.y + 12 * s), t.c_str(), 0xffffff, false);
    (void)bw;
    float blink = std::fmod(NowSec(), 1.0) < 0.5 ? 1.f : 0.25f;
    dl->AddCircleFilled(ImVec2(a.x + 12 * s + 13 * s, a.y + 12 * s + (ImGui::GetFontSize() + 8 * s) / 2), 4.5f * s, Col(cRec, blink));
  }
  if (comparing) {
    badge(ImVec2(a.x + 12 * s, b.y - 12 * s - ImGui::GetFontSize() - 8 * s), "ORIGINAL", 0xcfd3d8, false);
    badge(ImVec2(b.x - 12 * s, b.y - 12 * s - ImGui::GetFontSize() - 8 * s), "ENHANCED", cAccent2, true);
  }
  if (fullscreen && NowSec() < fsHintUntil) {
    const char* t = "Press Esc or F to exit fullscreen";
    ImVec2 ts = ImGui::CalcTextSize(t);
    badge(ImVec2(a.x + (w - ts.x - 18 * s) / 2, a.y + 20 * s), t, 0xffffff, false);
  }
  ImGui::PopFont();

  if (NowSec() < flashUntil) dl->AddRectFilled(a, b, Col(0xffffff, float((flashUntil - NowSec()) / 0.25 * 0.5)), rnd);

  // No-signal overlay
  if (!live) {
    std::string title, body;
    bool rescan = true;
    if (capErr == "nodevice") title = "No capture card found", body = "Plug in your Vivitar capture card over USB. It will connect automatically.";
    else if (capErr == "lost") title = "Capture card disconnected", body = "Reconnect it and it will come back automatically.";
    else if (!capErr.empty()) title = "Couldn't open the device", body = capErr;
    else if (opening.valid()) title = "Connecting", body = "Opening " + (curVideoName.empty() ? std::string("the capture card") : W2U(curVideoName)) + "...", rescan = false;
    else if (cap->IsOpen()) title = "Waiting for video", body = "Connected. Waiting for the first frame from the card.", rescan = false;
    else title = "Connecting", body = "Looking for your capture card.", rescan = false;
    ImGui::PushFont(fBig);
    ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    float cy = a.y + h / 2 - 40 * s;
    dl->AddText(ImVec2(a.x + (w - ts.x) / 2, cy), Col(cText), title.c_str());
    ImGui::PopFont();
    float bw = std::min(460 * s, w - 40 * s);
    ImGui::SetCursorScreenPos(ImVec2(a.x + (w - bw) / 2, cy + ts.y + 10 * s));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
    ImGui::PushStyleColor(ImGuiCol_Text, V4(cMuted));
    ImVec2 bs = ImGui::CalcTextSize(body.c_str(), nullptr, false, bw);
    ImGui::SetCursorScreenPos(ImVec2(a.x + (w - bs.x) / 2, cy + ts.y + 10 * s));
    ImGui::TextWrapped("%s", body.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (rescan) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + (w - 140 * s) / 2, ImGui::GetCursorScreenPos().y + 14 * s));
      ImGui::PushStyleColor(ImGuiCol_Button, V4(cAccent));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(cAccent2));
      if (ImGui::Button("Try again", ImVec2(140 * s, 32 * s))) {
        Scan();
        OpenDevice();
      }
      ImGui::PopStyleColor(2);
    }
  }
}

void App::Sidebar() {
  bool lockRec = recording;
  // ---- Device
  Section("DEVICE");
  auto nameOf = [](const std::vector<DeviceInfo>& l, const std::wstring& id, const char* none) -> std::string {
    for (auto& d : l)
      if (d.id == id) return W2U(d.name);
    return none;
  };
  ImGui::BeginDisabled(lockRec);
  std::wstring curVid = PickVideo();
  ImGui::TextColored(V4(cMuted), "Video input");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##vid", nameOf(vids, curVid, "None found").c_str())) {
    for (auto& d : vids)
      if (ImGui::Selectable(W2U(d.name).c_str(), d.id == curVid)) {
        S.videoId = d.id;
        Dirty();
        OpenDevice();
      }
    ImGui::EndCombo();
  }
  ImGui::TextColored(V4(cMuted), "Audio input");
  ImGui::SetNextItemWidth(-1);
  std::wstring curAud = PickAudio(curVideoName);
  std::string audLabel = nameOf(auds, curAud, "No audio");
  if (S.audioAuto) audLabel = "Auto: " + audLabel;
  if (ImGui::BeginCombo("##aud", audLabel.c_str())) {
    if (ImGui::Selectable("Auto (match the capture card)", S.audioAuto)) {
      S.audioAuto = true;
      Dirty();
      OpenDevice();
    }
    if (ImGui::Selectable("No audio", !S.audioAuto && S.audioId.empty())) {
      S.audioAuto = false;
      S.audioId.clear();
      Dirty();
      OpenDevice();
    }
    for (auto& d : auds)
      if (ImGui::Selectable(W2U(d.name).c_str(), !S.audioAuto && d.id == S.audioId)) {
        S.audioAuto = false;
        S.audioId = d.id;
        Dirty();
        OpenDevice();
      }
    ImGui::EndCombo();
  }
  if (ImGui::Button("Rescan devices", ImVec2(-1, 0))) {
    Scan();
    OpenDevice();
  }
  ImGui::EndDisabled();
  Divider(s);

  // ---- Video
  Section("VIDEO");
  ImGui::BeginDisabled(lockRec);
  std::set<std::pair<UINT32, UINT32>> resSet;
  for (auto& m : cap->modes) resSet.insert({m.w, m.h});
  if (resSet.empty()) resSet = {{1920, 1080}, {1280, 720}};
  std::vector<std::pair<UINT32, UINT32>> resList(resSet.rbegin(), resSet.rend());
  std::sort(resList.begin(), resList.end(), [](auto& x, auto& y) { return x.first * x.second > y.first * y.second; });
  float half = (ImGui::GetContentRegionAvail().x - 8 * s) / 2;
  ImGui::BeginGroup();
  ImGui::TextColored(V4(cMuted), "Resolution");
  char lbl[64];
  snprintf(lbl, sizeof(lbl), "%dx%d", S.width, S.height);
  ImGui::SetNextItemWidth(half);
  if (ImGui::BeginCombo("##res", lbl)) {
    for (auto& r : resList) {
      snprintf(lbl, sizeof(lbl), "%ux%u", r.first, r.second);
      if (ImGui::Selectable(lbl, (int)r.first == S.width && (int)r.second == S.height)) {
        S.width = r.first;
        S.height = r.second;
        Dirty();
        OpenDevice();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 8 * s);
  ImGui::BeginGroup();
  ImGui::TextColored(V4(cMuted), "Frame rate");
  std::set<int> fpsSet;
  for (auto& m : cap->modes)
    if ((int)m.w == S.width && (int)m.h == S.height) fpsSet.insert((int)std::lround(m.fps()));
  if (fpsSet.empty()) fpsSet = {60, 30};
  snprintf(lbl, sizeof(lbl), "%d fps", S.fps);
  ImGui::SetNextItemWidth(half);
  if (ImGui::BeginCombo("##fps", lbl)) {
    for (auto it = fpsSet.rbegin(); it != fpsSet.rend(); ++it) {
      snprintf(lbl, sizeof(lbl), "%d fps", *it);
      if (ImGui::Selectable(lbl, *it == S.fps)) {
        S.fps = *it;
        Dirty();
        OpenDevice();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndGroup();

  ImGui::TextColored(V4(cMuted), "Capture format");
  const char* fmts[] = {"Auto (best quality)", "Prefer uncompressed", "Prefer MJPEG"};
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##fmt", &S.fmtPref, fmts, 3)) {
    Dirty();
    OpenDevice();
  }
  {
    // What formats does this card offer at the chosen resolution?
    std::map<std::string, std::set<int>> byFmt;
    for (auto& m : cap->modes)
      if ((int)m.w == S.width && (int)m.h == S.height) byFmt[m.fmtName()].insert((int)std::lround(m.fps()));
    std::string avail;
    for (auto& [f, rates] : byFmt) {
      if (!avail.empty()) avail += ",  ";
      avail += f + " up to " + std::to_string(*rates.rbegin()) + " fps";
    }
    if (!avail.empty()) Muted(("Card offers at this resolution: " + avail).c_str());
  }
  ImGui::EndDisabled();

  ImGui::TextColored(V4(cMuted), "Colour range");
  {
    std::string autoL = std::string("Auto (") + (Color().fullRange ? "Full" : "Limited") + ")";
    int r = S.range;
    const char* items[] = {autoL.c_str(), "Limited", "Full"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##range", &r, items, 3)) {
      S.range = r;
      Dirty();
    }
    Muted("Blacks look grey? Try Limited. Dark areas crushed? Try Full.");
  }
  ImGui::TextColored(V4(cMuted), "Preview scaling");
  if (Seg("fit", &S.fit, {"Fit", "Fill", "Stretch", "1:1"})) Dirty();
  Divider(s);

  // ---- Enhancement
  Section("PICTURE ENHANCEMENT");
  ImGui::BeginDisabled(lockRec);
  if (Toggle("##enh", "Enhance picture", &S.enh)) Dirty();
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!S.enh);
  ImGui::Dummy(ImVec2(0, 4 * s));
  ImGui::TextColored(V4(cMuted), "Preset");
  int p = S.preset;
  if (Seg("preset", &p, {"Fast", "Subtle", "Balanced", "Strong", "Custom"})) ApplyPreset(p);
  ImGui::TextColored(V4(cMuted), "Upscaling");
  if (Seg("scaler", &S.scaler, {"Standard", "Bicubic", "Lanczos"})) S.preset = 4, Dirty();
  if (LabeledSlider("##sh", "Sharpen", &S.sharpen, 0, 100, "%d")) S.preset = 4, Dirty();
  if (LabeledSlider("##dn", "Noise & block reduction", &S.denoise, 0, 100, "%d")) S.preset = 4, Dirty();
  if (LabeledSlider("##br", "Brightness", &S.bri, -50, 50, "%+d")) Dirty();
  if (LabeledSlider("##co", "Contrast", &S.con, 50, 150, "%d%%")) Dirty();
  if (LabeledSlider("##sa", "Saturation", &S.sat, 0, 200, "%d%%")) Dirty();
  float bw = (ImGui::GetContentRegionAvail().x - 8 * s) / 2;
  ImGui::PushStyleColor(ImGuiCol_Border, compare ? V4(cAccent) : V4(cLine2));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
  if (ImGui::Button(compare ? "Comparing (C)" : "Compare (C)", ImVec2(bw, 30 * s))) compare = !compare;
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 8 * s);
  if (ImGui::Button("Reset", ImVec2(bw, 30 * s))) {
    S.bri = 0;
    S.con = 100;
    S.sat = 100;
    ApplyPreset(2);
  }
  ImGui::BeginDisabled(lockRec);
  if (Toggle("##enhrec", "Apply to recordings & screenshots", &S.enhRecord)) Dirty();
  ImGui::TextColored(V4(cMuted), "Output resolution");
  const char* outs[] = {"Same as source", "1440p (upscaled)"};
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##out", &S.outRes, outs, 2)) Dirty();
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  Divider(s);

  // ---- Audio
  Section("AUDIO");
  if (Toggle("##mon", "Monitor audio through speakers", &S.monitor)) {
    UpdateMonitorVolume();
    Dirty();
  }
  Muted("Turn off if you already hear the console through a TV or headphones, to avoid an echo.");
  Divider(s);

  // ---- Recording
  Section("RECORDING");
  ImGui::BeginDisabled(lockRec);
  ImGui::TextColored(V4(cMuted), "Quality");
  int qi = S.bitrate <= 8 ? 0 : S.bitrate <= 15 ? 1 : S.bitrate <= 30 ? 2 : 3;
  const char* qs[] = {"Low - 6 Mbps", "Medium - 12 Mbps", "High - 20 Mbps", "Max - 40 Mbps"};
  static const int qv[] = {6, 12, 20, 40};
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##q", &qi, qs, 4)) S.bitrate = qv[qi], Dirty();
  ImGui::EndDisabled();
  ImGui::TextColored(V4(cMuted), "Save to");
  ImGui::PushStyleColor(ImGuiCol_Text, V4(cText));
  ImGui::TextWrapped("%s", W2U(S.saveDir).c_str());
  ImGui::PopStyleColor();
  bw = (ImGui::GetContentRegionAvail().x - 8 * s) / 2;
  ImGui::BeginDisabled(lockRec);
  if (ImGui::Button("Change...", ImVec2(bw, 0))) ChooseFolder();
  ImGui::EndDisabled();
  ImGui::SameLine(0, 8 * s);
  if (ImGui::Button("Open folder", ImVec2(bw, 0))) {
    SHCreateDirectoryExW(nullptr, S.saveDir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", S.saveDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  Divider(s);

  // ---- Signal
  Section("SIGNAL");
  auto row = [&](const char* k, const std::string& v) {
    ImGui::TextColored(V4(cMuted), "%s", k);
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(std::max(90 * s, w - ImGui::CalcTextSize(v.c_str()).x));
    ImGui::TextUnformatted(v.c_str());
  };
  char b[128];
  row("Device", cap->IsOpen() ? W2U(curVideoName) : "-");
  if (cap->IsOpen()) snprintf(b, sizeof(b), "%ux%u @ %.2f %s", cap->cur.w, cap->cur.h, cap->cur.fps(), cap->cur.fmtName().c_str());
  row("Mode", cap->IsOpen() ? b : "-");
  snprintf(b, sizeof(b), "%.1f fps", measuredFps);
  row("Frame rate", cap->IsOpen() ? b : "-");
  ColorParams cp = Color();
  row("Colour", std::string(cp.fullRange ? "Full" : "Limited") + (cp.bt709 ? " - BT.709" : " - BT.601"));
  row("Audio", acap->IsOpen() ? acap->formatDesc : (audErr.empty() ? "None" : "Error"));
  row("GPU", g_gpuName);
  row("Encoder", rec.encoderName.empty() ? "H.264 / AAC (MP4)" : rec.encoderName);
  Divider(s);

  Section("SHORTCUTS");
  const char* keys[][2] = {{"Start / stop recording", "R"}, {"Screenshot", "S"},        {"Fullscreen preview", "F"},
                           {"Mute monitor", "M"},           {"Enhance on / off", "E"}, {"Compare before / after", "C"}};
  for (auto& k : keys) row(k[0], k[1]);
  ImGui::Dummy(ImVec2(0, 10 * s));
}

void App::BottomBar(float h) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetWindowSize().x;
  dl->AddRectFilled(o, ImVec2(o.x + W, o.y + h), Col(cPanel));
  dl->AddLine(o, ImVec2(o.x + W, o.y), Col(cLine));
  float cy = o.y + h / 2;

  // mute button + volume + meters
  ImGui::SetCursorScreenPos(ImVec2(o.x + 16 * s, cy - 17 * s));
  if (ImGui::InvisibleButton("mute", ImVec2(34 * s, 34 * s))) {
    if (!S.monitor) S.monitor = true, S.muted = false;
    else S.muted = !S.muted;
    UpdateMonitorVolume();
    Dirty();
  }
  bool audible = S.monitor && !S.muted;
  if (ImGui::IsItemHovered()) {
    dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Col(cPanel2), 8 * s);
    ImGui::SetTooltip(audible ? "Mute (M)" : "Unmute (M)");
  }
  {
    ImVec2 c(o.x + 33 * s, cy);
    uint32_t col = audible ? cAccent2 : cMuted;
    dl->AddRectFilled(ImVec2(c.x - 8 * s, c.y - 3 * s), ImVec2(c.x - 4 * s, c.y + 3 * s), Col(col));
    dl->AddTriangleFilled(ImVec2(c.x - 4 * s, c.y - 3 * s), ImVec2(c.x + 1 * s, c.y - 7 * s), ImVec2(c.x + 1 * s, c.y + 7 * s), Col(col));
    dl->AddTriangleFilled(ImVec2(c.x - 4 * s, c.y - 3 * s), ImVec2(c.x + 1 * s, c.y + 7 * s), ImVec2(c.x - 4 * s, c.y + 3 * s), Col(col));
    if (audible) {
      dl->PathArcTo(ImVec2(c.x + 1 * s, c.y), 5 * s, -0.9f, 0.9f);
      dl->PathStroke(Col(col), 0, 1.5f * s);
      dl->PathArcTo(ImVec2(c.x + 1 * s, c.y), 9 * s, -0.9f, 0.9f);
      dl->PathStroke(Col(col), 0, 1.5f * s);
    } else {
      dl->AddLine(ImVec2(c.x + 4 * s, c.y - 4 * s), ImVec2(c.x + 10 * s, c.y + 4 * s), Col(col), 1.6f * s);
      dl->AddLine(ImVec2(c.x + 10 * s, c.y - 4 * s), ImVec2(c.x + 4 * s, c.y + 4 * s), Col(col), 1.6f * s);
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x + 58 * s, cy - ImGui::GetFrameHeight() / 2));
  ImGui::SetNextItemWidth(100 * s);
  if (ImGui::SliderInt("##vol", &S.volume, 0, 100, "")) {
    if (S.muted && S.volume > 0) S.muted = false;
    UpdateMonitorVolume();
    Dirty();
  }
  float mx = o.x + 172 * s, mw = 180 * s;
  auto meter = [&](float y, float v, float pk, const char* ch) {
    dl->AddText(fBold, 10 * s, ImVec2(mx, y - 6 * s), Col(cDim), ch);
    ImVec2 a(mx + 12 * s, y - 3 * s), b(mx + 12 * s + mw, y + 3 * s);
    dl->AddRectFilled(a, b, Col(0x0b0c0e), 3 * s);
    float len = (b.x - a.x);
    float g = std::min(v, 0.7f), yl = std::clamp(v - 0.7f, 0.f, 0.2f), r = std::clamp(v - 0.9f, 0.f, 0.1f);
    if (g > 0) dl->AddRectFilled(a, ImVec2(a.x + len * g, b.y), Col(cOk), 3 * s);
    if (yl > 0) dl->AddRectFilled(ImVec2(a.x + len * 0.7f, a.y), ImVec2(a.x + len * (0.7f + yl), b.y), Col(0xf5d323));
    if (r > 0) dl->AddRectFilled(ImVec2(a.x + len * 0.9f, a.y), ImVec2(a.x + len * (0.9f + r), b.y), Col(cRec));
    if (pk > 0.01f) dl->AddRectFilled(ImVec2(a.x + len * pk - 2 * s, a.y), ImVec2(a.x + len * pk, b.y), Col(0xffffff, 0.8f));
  };
  meter(cy - 6 * s, meterL, peakHoldL, "L");
  meter(cy + 6 * s, meterR, peakHoldR, "R");

  // centre: screenshot, record, timer
  bool live = cap->IsOpen() && R.HasFrame();
  float rx = o.x + W / 2;
  ImGui::SetCursorScreenPos(ImVec2(rx - 30 * s - 22 * s - 44 * s, cy - 22 * s));
  ImGui::BeginDisabled(!live);
  if (ImGui::InvisibleButton("shot", ImVec2(44 * s, 44 * s))) Screenshot();
  ImGui::EndDisabled();
  {
    ImVec2 c((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, cy);
    bool hov = ImGui::IsItemHovered();
    dl->AddCircleFilled(c, 22 * s, hov && live ? Col(cLine) : Col(cPanel2));
    dl->AddCircle(c, 22 * s, Col(cLine2), 0, 1);
    uint32_t ic = live ? cText : cDim;
    dl->AddRect(ImVec2(c.x - 9 * s, c.y - 6 * s), ImVec2(c.x + 9 * s, c.y + 7 * s), Col(ic), 2 * s, 0, 1.6f * s);
    dl->AddRectFilled(ImVec2(c.x - 4 * s, c.y - 9 * s), ImVec2(c.x + 4 * s, c.y - 6 * s), Col(ic));
    dl->AddCircle(ImVec2(c.x, c.y + 0.5f * s), 3.5f * s, Col(ic), 0, 1.6f * s);
    if (hov) ImGui::SetTooltip("Screenshot (S)");
  }
  ImGui::SetCursorScreenPos(ImVec2(rx - 30 * s, cy - 30 * s));
  ImGui::BeginDisabled(!live && !recording);
  if (ImGui::InvisibleButton("rec", ImVec2(60 * s, 60 * s))) recording ? StopRecording() : StartRecording();
  ImGui::EndDisabled();
  {
    ImVec2 c(rx, cy);
    bool hov = ImGui::IsItemHovered();
    float alpha = live || recording ? 1.f : 0.35f;
    if (recording) {
      float t = (float)std::fmod(NowSec(), 1.6) / 1.6f;
      dl->AddCircle(c, 30 * s + t * 12 * s, Col(cRec, 0.5f * (1 - t)), 0, 3 * s);
      dl->AddCircle(c, 28.5f * s, Col(cRec), 0, 3 * s);
      dl->AddRectFilled(ImVec2(c.x - 11 * s, c.y - 11 * s), ImVec2(c.x + 11 * s, c.y + 11 * s), Col(cRec), 5 * s);
    } else {
      dl->AddCircle(c, 28.5f * s, Col(0xe9ebee, alpha), 0, 3 * s);
      dl->AddCircleFilled(c, (hov ? 23 : 22) * s, Col(cRec, alpha));
    }
    if (hov) ImGui::SetTooltip(recording ? "Stop recording (R)" : "Record (R)");
  }
  ImGui::PushFont(fBig);
  std::string tm = recording ? FmtTime(Now100ns() - recStart) : "00:00:00";
  float tx = rx + 30 * s + 22 * s;
  dl->AddText(ImVec2(tx, cy - 18 * s), recording ? Col(cText) : Col(cMuted), tm.c_str());
  ImGui::PopFont();
  std::string sub = "Ready";
  if (recording) {
    sub = FmtSize(recBytes) + (recEnh ? "  Enhanced" : "") + "  " + std::to_string(rec.cfg.h) + "p";
    if (rec.dropped) sub += "  " + std::to_string(rec.dropped.load()) + " dropped";
  }
  dl->AddText(ImVec2(tx, cy + 8 * s), Col(cDim), sub.c_str());

  // right: open folder + fullscreen
  float fx = o.x + W - 16 * s - 34 * s;
  ImGui::SetCursorScreenPos(ImVec2(fx, cy - 17 * s));
  if (ImGui::InvisibleButton("full", ImVec2(34 * s, 34 * s))) ToggleFullscreen();
  {
    ImVec2 c((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, cy);
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Col(cPanel2), 8 * s);
      ImGui::SetTooltip("Fullscreen (F)");
    }
    float k = 7 * s, l = 4 * s;
    uint32_t col = cMuted;
    float th = 1.6f * s;
    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x - k + l, c.y - k), Col(col), th);
    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x - k, c.y - k + l), Col(col), th);
    dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x + k - l, c.y - k), Col(col), th);
    dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x + k, c.y - k + l), Col(col), th);
    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x - k + l, c.y + k), Col(col), th);
    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x - k, c.y + k - l), Col(col), th);
    dl->AddLine(ImVec2(c.x + k, c.y + k), ImVec2(c.x + k - l, c.y + k), Col(col), th);
    dl->AddLine(ImVec2(c.x + k, c.y + k), ImVec2(c.x + k, c.y + k - l), Col(col), th);
  }
}

void App::Library() {
  if (libDirty) {
    ScanLibrary();
    libDirty = false;
  }
  ImGui::SetCursorPos(ImVec2(22 * s, 18 * s));
  ImGui::BeginGroup();
  ImGui::PushFont(fBig);
  ImGui::TextUnformatted("Library");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s);
  ImGui::TextColored(V4(cMuted), "%d captures in %s", (int)lib.size(), W2U(S.saveDir).c_str());
  ImGui::EndGroup();
  float W = ImGui::GetWindowSize().x;
  ImGui::SetCursorPos(ImVec2(W - 22 * s - 250 * s, 20 * s));
  if (ImGui::Button("Refresh", ImVec2(80 * s, 30 * s))) libDirty = true;
  ImGui::SameLine(0, 8 * s);
  if (ImGui::Button("Open folder", ImVec2(162 * s, 30 * s))) {
    SHCreateDirectoryExW(nullptr, S.saveDir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", S.saveDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SetCursorPos(ImVec2(22 * s, 70 * s));
  if (lib.empty()) {
    ImGui::SetCursorPos(ImVec2(22 * s, 120 * s));
    Muted("No captures yet. Press the record button or R to start, or S for a screenshot.");
    return;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10 * s, 8 * s));
  ImGui::BeginChild("libtable", ImVec2(W - 44 * s, ImGui::GetContentRegionAvail().y - 14 * s));
  if (ImGui::BeginTable("lib", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70 * s);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90 * s);
    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150 * s);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 250 * s);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)lib.size(); i++) {
      auto& it = lib[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(W2U(it.name).c_str());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(it.video ? V4(cAccent2) : V4(cWarn), it.video ? "VIDEO" : "IMAGE");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(FmtSize(it.size).c_str());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      FILETIME lt;
      SYSTEMTIME st;
      FileTimeToLocalFileTime(&it.mtime, &lt);
      FileTimeToSystemTime(&lt, &st);
      ImGui::Text("%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
      ImGui::TableNextColumn();
      if (ImGui::Button(it.video ? "Play" : "View", ImVec2(70 * s, 0)))
        ShellExecuteW(nullptr, L"open", it.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      ImGui::SameLine();
      if (ImGui::Button("Show", ImVec2(70 * s, 0))) {
        std::wstring args = L"/select,\"" + it.path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete", ImVec2(70 * s, 0))) {
        pendingDelete = i;
        ImGui::OpenPopup("Delete capture?");
      }
      if (ImGui::BeginPopupModal("Delete capture?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Move \"%s\" to the Recycle Bin?", W2U(lib[pendingDelete].name).c_str());
        ImGui::Dummy(ImVec2(0, 6 * s));
        if (ImGui::Button("Move to Recycle Bin", ImVec2(180 * s, 0))) {
          std::wstring from = lib[pendingDelete].path;
          from.push_back(L'\0');
          SHFILEOPSTRUCTW op{};
          op.wFunc = FO_DELETE;
          op.pFrom = from.c_str();
          op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
          if (SHFileOperationW(&op) == 0) ToastMsg("Moved to the Recycle Bin");
          else ToastMsg("Couldn't delete the file", cRec);
          libDirty = true;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void App::DrawToasts() {
  std::lock_guard<std::mutex> lk(toastM);
  double now = NowSec();
  toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) { return t.until < now; }), toasts.end());
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 vs = ImGui::GetMainViewport()->Size;
  float y = vs.y - (fullscreen ? 30 : 100) * s;
  float maxW = std::min(560 * s, vs.x - 40 * s);
  for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
    ImVec2 ts = ImGui::CalcTextSize(it->text.c_str(), nullptr, false, maxW - 32 * s);
    float w = ts.x + 32 * s, h = ts.y + 20 * s;
    ImVec2 a((vs.x - w) / 2, y - h);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Col(0x262b33), 9 * s);
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), it->color == cText ? Col(cLine2) : Col(it->color, 0.6f), 9 * s);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 16 * s, a.y + 10 * s), Col(cText), it->text.c_str(), nullptr,
                maxW - 32 * s);
    y -= h + 8 * s;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Win32 plumbing

static App* g_app = nullptr;

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) {
        g_resizeW = LOWORD(lParam);
        g_resizeH = HIWORD(lParam);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
      mm->ptMinTrackSize.x = 900;
      mm->ptMinTrackSize.y = 600;
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<RECT*>(lParam);
      SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_CLOSE:
      if (g_app) g_app->OnClose();
      DestroyWindow(hWnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void SetupStyle(float s) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  st.WindowRounding = 8;
  st.ChildRounding = 0;
  st.FrameRounding = 6;
  st.PopupRounding = 8;
  st.GrabRounding = 6;
  st.ScrollbarRounding = 6;
  st.FramePadding = ImVec2(10, 6);
  st.ItemSpacing = ImVec2(8, 7);
  st.WindowBorderSize = 1;
  st.PopupBorderSize = 1;
  st.FrameBorderSize = 0;
  st.ScrollbarSize = 10;
  st.GrabMinSize = 12;
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = V4(cText);
  c[ImGuiCol_TextDisabled] = V4(cDim);
  c[ImGuiCol_WindowBg] = V4(cBg);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = V4(0x20242b);
  c[ImGuiCol_Border] = V4(cLine2);
  c[ImGuiCol_FrameBg] = V4(cPanel2);
  c[ImGuiCol_FrameBgHovered] = V4(cLine);
  c[ImGuiCol_FrameBgActive] = V4(cLine2);
  c[ImGuiCol_Button] = V4(cPanel2);
  c[ImGuiCol_ButtonHovered] = V4(cLine);
  c[ImGuiCol_ButtonActive] = V4(cLine2);
  c[ImGuiCol_Header] = V4(cAccent, 0.25f);
  c[ImGuiCol_HeaderHovered] = V4(cAccent, 0.35f);
  c[ImGuiCol_HeaderActive] = V4(cAccent, 0.45f);
  c[ImGuiCol_SliderGrab] = V4(0xffffff);
  c[ImGuiCol_SliderGrabActive] = V4(cAccent2);
  c[ImGuiCol_CheckMark] = V4(cAccent);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = V4(cLine2);
  c[ImGuiCol_TableHeaderBg] = V4(cPanel);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = V4(0xffffff, 0.02f);
  c[ImGuiCol_TableBorderLight] = V4(cLine);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
  st.ScaleAllSizes(s);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  LogInit();
  Log("Vivitar Capture starting");
  MFStartup(MF_VERSION);
  ImGui_ImplWin32_EnableDpiAwareness();

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"VivitarCaptureWnd";
  RegisterClassExW(&wc);

  POINT pt{0, 0};
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  int ww = std::min(int(1400 * scale), int(mi.rcWork.right - mi.rcWork.left));
  int wh = std::min(int(880 * scale), int(mi.rcWork.bottom - mi.rcWork.top));
  int wx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
  int wy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Vivitar Capture", WS_OVERLAPPEDWINDOW, wx, wy, ww, wh, nullptr, nullptr,
                           hInst, nullptr);
  BOOL dark = TRUE;
  DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

  if (!CreateDeviceD3D(g_hwnd)) {
    MessageBoxW(nullptr, L"Couldn't start Direct3D 11. Please update your graphics driver.", L"Vivitar Capture", MB_ICONERROR);
    return 1;
  }
  ShowWindow(g_hwnd, SW_SHOWDEFAULT);
  UpdateWindow(g_hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  scale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
  App app;
  app.s = scale;
  auto loadFont = [&](const char* file, float size) -> ImFont* {
    char path[MAX_PATH];
    GetWindowsDirectoryA(path, MAX_PATH);
    std::string p = std::string(path) + "\\Fonts\\" + file;
    if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return io.Fonts->AddFontFromFileTTF(p.c_str(), size * scale);
    ImFontConfig cfg;
    cfg.SizePixels = size * scale;
    return io.Fonts->AddFontDefault(&cfg);
  };
  app.fBody = loadFont("segoeui.ttf", 15.f);
  app.fBold = loadFont("segoeuib.ttf", 13.f);
  app.fBig = loadFont("seguisb.ttf", 21.f);
  SetupStyle(scale);
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());

  g_app = &app;
  app.Init();

  bool done = false;
  while (!done) {
    if (g_frameWait) WaitForSingleObjectEx(g_frameWait, 100, TRUE);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (g_resizeW && g_resizeH) {
      g_rtv.Reset();
      g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, kSwapFlags);
      g_resizeW = g_resizeH = 0;
      CreateRTV();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.Tick();
    if (IsIconic(g_hwnd)) {
      ImGui::EndFrame();
      Sleep(8);
      continue;
    }
    app.UI();
    ImGui::Render();
    app.RenderPreview();
    const float clear[4] = {0.06f, 0.07f, 0.08f, 1.f};
    g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{0, 0, io.DisplaySize.x, io.DisplaySize.y, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
  }

  app.Shutdown();
  g_app = nullptr;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  MFShutdown();
  CoUninitialize();
  return 0;
}
