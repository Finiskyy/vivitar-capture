#include "capture.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <tuple>

static const DWORD VID = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
static const DWORD AUD = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;

std::vector<DeviceInfo> EnumDevices(bool audio) {
  std::vector<DeviceInfo> out;
  ComPtr<IMFAttributes> a;
  if (FAILED(MFCreateAttributes(&a, 1))) return out;
  a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
             audio ? MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID : MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devs = nullptr;
  UINT32 n = 0;
  HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
  if (FAILED(hr)) {
    Log("MFEnumDeviceSources(%s) failed: %s", audio ? "audio" : "video", HrStr(hr).c_str());
    return out;
  }
  for (UINT32 i = 0; i < n; i++) {
    WCHAR* name = nullptr;
    WCHAR* id = nullptr;
    UINT32 len = 0;
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
    devs[i]->GetAllocatedString(audio ? MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID
                                      : MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                &id, &len);
    if (id) out.push_back({name ? name : L"Unknown device", id});
    CoTaskMemFree(name);
    CoTaskMemFree(id);
    devs[i]->Release();
  }
  CoTaskMemFree(devs);
  return out;
}

bool VideoMode::compressed() const {
  return subtype == MFVideoFormat_MJPG || subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC;
}

std::string VideoMode::fmtName() const {
  if (subtype == MFVideoFormat_MJPG) return "MJPEG";
  if (subtype == MFVideoFormat_YUY2) return "YUY2";
  if (subtype == MFVideoFormat_NV12) return "NV12";
  if (subtype == MFVideoFormat_UYVY) return "UYVY";
  if (subtype == MFVideoFormat_I420) return "I420";
  if (subtype == MFVideoFormat_YV12) return "YV12";
  if (subtype == MFVideoFormat_RGB24) return "RGB24";
  if (subtype == MFVideoFormat_RGB32) return "RGB32";
  if (subtype == MFVideoFormat_H264) return "H.264";
  if (subtype == MFVideoFormat_HEVC) return "HEVC";
  char b[16];
  snprintf(b, sizeof(b), "%c%c%c%c", (char)(subtype.Data1 & 0xff), (char)((subtype.Data1 >> 8) & 0xff),
           (char)((subtype.Data1 >> 16) & 0xff), (char)((subtype.Data1 >> 24) & 0xff));
  return b;
}

// ---------------------------------------------------------------------------------------------------------------
// Video

bool VideoCapture::Open(const CaptureRequest& req, std::string& err) {
  Close();
  stop_ = false;
  lost_ = false;
  std::promise<std::string> prom;
  auto fut = prom.get_future();
  th_ = std::thread([this, req, &prom] { Run(req, &prom); });
  err = fut.get();
  if (!err.empty()) {
    if (th_.joinable()) th_.join();
    return false;
  }
  return true;
}

void VideoCapture::Close() {
  stop_ = true;
  {
    std::lock_guard<std::mutex> lk(srcMutex_);
    if (source_) {
      ComPtr<IMFMediaSource> s;
      if (SUCCEEDED(source_.As(&s))) s->Shutdown();  // unblocks a pending ReadSample
    }
  }
  if (th_.joinable()) th_.join();
  std::lock_guard<std::mutex> lk(qm_);
  q_.clear();
}

void VideoCapture::TakeFrames(std::vector<VideoFrame>& out) {
  std::lock_guard<std::mutex> lk(qm_);
  while (!q_.empty()) {
    out.push_back(std::move(q_.front()));
    q_.pop_front();
  }
}

void VideoCapture::Recycle(std::vector<VideoFrame>& frames) {
  std::lock_guard<std::mutex> lk(qm_);
  for (auto& f : frames)
    if (pool_.size() < 8) pool_.push_back(std::move(f));
  frames.clear();
}

static std::string CaptureError(HRESULT hr) {
  if (hr == E_ACCESSDENIED)
    return "Windows blocked camera access. Turn on Settings > Privacy & security > Camera > \"Let desktop apps access "
           "your camera\", then try again.";
  if (hr == (HRESULT)0xC00D3704 /* MF_E_HW_MFT_FAILED_START_STREAMING */ ||
      hr == (HRESULT)0xC00DABE4 /* recording device preempted */ ||
      hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_BUSY))
    return "The capture card is being used by another program. Close OBS, the Camera app, Discord or Zoom and try "
           "again.";
  return "Couldn't open the capture card (" + HrStr(hr) + ").";
}

void VideoCapture::Run(CaptureRequest req, std::promise<std::string>* prom) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto finish = [&](const std::string& e) {
    if (prom) {
      prom->set_value(e);
      prom = nullptr;
    }
  };

  ComPtr<IMFMediaSource> src;
  ComPtr<IMFSourceReader> reader;
  HRESULT hr = S_OK;
  std::string setupErr = [&]() -> std::string {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 2);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, req.id.c_str());
    hr = MFCreateDeviceSource(a.Get(), &src);
    if (FAILED(hr)) return CaptureError(hr);
    {
      std::lock_guard<std::mutex> lk(srcMutex_);
      source_ = src;
    }
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 2);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    hr = MFCreateSourceReaderFromMediaSource(src.Get(), ra.Get(), &reader);
    if (FAILED(hr)) return CaptureError(hr);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(VID, TRUE);

    // Enumerate every native mode.
    std::vector<std::pair<VideoMode, DWORD>> all;
    for (DWORD i = 0;; i++) {
      ComPtr<IMFMediaType> t;
      if (FAILED(reader->GetNativeMediaType(VID, i, &t))) break;
      VideoMode m;
      t->GetGUID(MF_MT_SUBTYPE, &m.subtype);
      MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &m.w, &m.h);
      MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &m.fpsN, &m.fpsD);
      if (!m.w || !m.h) continue;
      all.push_back({m, i});
    }
    if (all.empty()) return "The device didn't report any video formats.";
    modes.clear();
    for (auto& p : all) modes.push_back(p.first);
    for (auto& m : modes) Log("  mode %ux%u @ %.2f %s", m.w, m.h, m.fps(), m.fmtName().c_str());

    // Pick: closest resolution, then closest frame rate, then format preference.
    double reqArea = double(req.w) * req.h;
    bool anyUncompressed = std::any_of(all.begin(), all.end(), [](auto& p) { return !p.first.compressed(); });
    auto key = [&](const VideoMode& m) {
      double resDiff = (m.w == req.w && m.h == req.h) ? 0 : 1 + std::fabs(double(m.w) * m.h - reqArea) / reqArea;
      double fpsDiff = std::fabs(m.fps() - req.fps);
      if (fpsDiff < 0.6) fpsDiff = 0;  // 59.94 == 60
      int fmt = 0;
      if (req.fmtPref == 2) fmt = m.compressed() ? 0 : 1;
      else fmt = m.compressed() ? 1 : 0;
      if (req.fmtPref == 1 && anyUncompressed && m.compressed()) resDiff += 1000;  // uncompressed only
      // Prefer NV12/YUY2 over RGB among uncompressed (cheaper to convert).
      int sub = (m.subtype == MFVideoFormat_NV12 || m.subtype == MFVideoFormat_YUY2) ? 0 : 1;
      return std::tuple<double, double, int, int>(resDiff, fpsDiff, fmt, sub);
    };
    auto best = std::min_element(all.begin(), all.end(), [&](auto& a1, auto& b1) { return key(a1.first) < key(b1.first); });
    cur = best->first;
    ComPtr<IMFMediaType> native;
    reader->GetNativeMediaType(VID, best->second, &native);
    hr = reader->SetCurrentMediaType(VID, nullptr, native.Get());
    if (FAILED(hr)) return "Couldn't select " + cur.fmtName() + " mode (" + HrStr(hr) + ").";

    UINT32 nr = 0;
    if (SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nr)))
      nominalRange = nr == MFNominalRange_0_255 ? 1 : nr == MFNominalRange_16_235 ? 2 : 0;
    else
      nominalRange = 0;

    // Ask the reader for NV12 (it inserts the MJPEG decoder / colour converter as needed).
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, cur.w, cur.h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, cur.fpsN, cur.fpsD);
    hr = reader->SetCurrentMediaType(VID, nullptr, out.Get());
    if (FAILED(hr)) return "Couldn't convert " + cur.fmtName() + " video (" + HrStr(hr) + ").";
    ComPtr<IMFMediaType> got;
    reader->GetCurrentMediaType(VID, &got);
    MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &outW, &outH);
    if (SUCCEEDED(got->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nr)) && nominalRange == 0)
      nominalRange = nr == MFNominalRange_0_255 ? 1 : nr == MFNominalRange_16_235 ? 2 : 0;
    Log("Video opened: %ux%u @ %.2f %s -> NV12 %ux%u, range=%d", cur.w, cur.h, cur.fps(), cur.fmtName().c_str(), outW,
        outH, nominalRange);
    return {};
  }();

  if (!setupErr.empty()) {
    Log("Video open failed: %s", setupErr.c_str());
    if (src) src->Shutdown();
    {
      std::lock_guard<std::mutex> lk(srcMutex_);
      source_.Reset();
    }
    finish(setupErr);
    CoUninitialize();
    return;
  }
  finish({});

  UINT32 W = outW, H = outH;
  while (!stop_) {
    DWORD si = 0, flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> s;
    hr = reader->ReadSample(VID, 0, &si, &flags, &ts, &s);
    if (stop_) break;
    if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
      Log("Video ReadSample stopped: hr=%s flags=0x%x", HrStr(hr).c_str(), flags);
      lost_ = true;
      break;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      ComPtr<IMFMediaType> got;
      if (SUCCEEDED(reader->GetCurrentMediaType(VID, &got))) MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &W, &H);
      outW = W;
      outH = H;
      Log("Video size changed to %ux%u", W, H);
    }
    if (!s) continue;
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) continue;

    VideoFrame f;
    {
      std::lock_guard<std::mutex> lk(qm_);
      if (!pool_.empty()) {
        f = std::move(pool_.back());
        pool_.pop_back();
      }
    }
    f.w = W;
    f.h = H;
    f.t = Now100ns();
    f.data.resize(size_t(W) * H * 3 / 2);

    BYTE* p = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    ComPtr<IMF2DBuffer> b2;
    bool locked2d = SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
    if (locked2d) {
      b2->GetContiguousLength(&len);
    } else {
      DWORD maxLen = 0;
      if (FAILED(b->Lock(&p, &maxLen, &len))) continue;
      pitch = (LONG)W;
    }
    if (pitch < (LONG)W) pitch = (LONG)W;
    UINT32 alignedH = (H + 15) & ~15u;
    UINT32 planeH = (alignedH != H && len == DWORD(pitch) * alignedH * 3 / 2) ? alignedH : H;
    bool ok = len == 0 || len >= DWORD(pitch) * (planeH + H / 2);
    if (ok) {
      uint8_t* dst = f.data.data();
      for (UINT32 y = 0; y < H; y++) memcpy(dst + size_t(y) * W, p + size_t(y) * pitch, W);
      const BYTE* uv = p + size_t(pitch) * planeH;
      uint8_t* duv = dst + size_t(W) * H;
      for (UINT32 y = 0; y < H / 2; y++) memcpy(duv + size_t(y) * W, uv + size_t(y) * pitch, W);
    }
    if (locked2d) b2->Unlock2D();
    else b->Unlock();
    if (!ok) continue;

    {
      std::lock_guard<std::mutex> lk(cbMutex_);
      if (onFrame_) onFrame_(f);
    }
    std::lock_guard<std::mutex> lk(qm_);
    while (q_.size() >= 4) {
      if (pool_.size() < 8) pool_.push_back(std::move(q_.front()));
      q_.pop_front();
    }
    q_.push_back(std::move(f));
  }

  reader.Reset();
  if (src) src->Shutdown();
  {
    std::lock_guard<std::mutex> lk(srcMutex_);
    source_.Reset();
  }
  CoUninitialize();
}

// ---------------------------------------------------------------------------------------------------------------
// Audio

bool AudioCapture::Open(const std::wstring& id, std::string& err) {
  Close();
  stop_ = false;
  lost_ = false;
  std::promise<std::string> prom;
  auto fut = prom.get_future();
  th_ = std::thread([this, id, &prom] { Run(id, &prom); });
  err = fut.get();
  if (!err.empty()) {
    if (th_.joinable()) th_.join();
    return false;
  }
  return true;
}

void AudioCapture::Close() {
  stop_ = true;
  {
    std::lock_guard<std::mutex> lk(srcMutex_);
    if (source_) {
      ComPtr<IMFMediaSource> s;
      if (SUCCEEDED(source_.As(&s))) s->Shutdown();
    }
  }
  if (th_.joinable()) th_.join();
  peakL = 0;
  peakR = 0;
}

namespace {
// Converts any PCM / float layout to interleaved stereo float, then resamples linearly to 48 kHz.
struct Converter {
  int ch = 2, bits = 16, rate = 48000;
  bool isFloat = false;
  double pos = 0;
  float lastL = 0, lastR = 0;
  std::vector<float> tmp;

  void Setup(const WAVEFORMATEX* wf) {
    ch = std::max<int>(1, wf->nChannels);
    bits = wf->wBitsPerSample;
    rate = (int)wf->nSamplesPerSec;
    isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
      auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
      isFloat = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
      if (ex->Samples.wValidBitsPerSample && !isFloat) bits = wf->wBitsPerSample;
    }
  }
  float Sample(const uint8_t* p) const {
    if (isFloat) return bits == 64 ? float(*reinterpret_cast<const double*>(p)) : *reinterpret_cast<const float*>(p);
    switch (bits) {
      case 8: return (float(*p) - 128.f) / 128.f;
      case 16: return *reinterpret_cast<const int16_t*>(p) / 32768.f;
      case 24: {
        int32_t v = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 24);
        return float(v) / 2147483648.f;
      }
      case 32: return float(*reinterpret_cast<const int32_t*>(p)) / 2147483648.f;
    }
    return 0.f;
  }
  // Returns stereo float frames at 48 kHz appended to `out`.
  void Process(const uint8_t* data, size_t bytes, std::vector<float>& out) {
    size_t bps = bits / 8, frame = bps * ch;
    if (!frame) return;
    size_t n = bytes / frame;
    tmp.resize(n * 2);
    for (size_t i = 0; i < n; i++) {
      const uint8_t* f = data + i * frame;
      float l = Sample(f);
      float r = ch > 1 ? Sample(f + bps) : l;
      tmp[i * 2] = l;
      tmp[i * 2 + 1] = r;
    }
    if (rate == 48000) {
      out.insert(out.end(), tmp.begin(), tmp.end());
      return;
    }
    double step = double(rate) / 48000.0;
    // index 0 = last sample of previous chunk, index k = tmp[k-1]
    while (pos < double(n)) {
      size_t i = (size_t)pos;
      double fr = pos - double(i);
      float l0 = i == 0 ? lastL : tmp[(i - 1) * 2], r0 = i == 0 ? lastR : tmp[(i - 1) * 2 + 1];
      float l1 = tmp[i * 2], r1 = tmp[i * 2 + 1];
      out.push_back(l0 + (l1 - l0) * float(fr));
      out.push_back(r0 + (r1 - r0) * float(fr));
      pos += step;
    }
    pos -= double(n);
    if (n) {
      lastL = tmp[(n - 1) * 2];
      lastR = tmp[(n - 1) * 2 + 1];
    }
  }
};
}  // namespace

void AudioCapture::Run(std::wstring id, std::promise<std::string>* prom) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto finish = [&](const std::string& e) {
    if (prom) {
      prom->set_value(e);
      prom = nullptr;
    }
  };
  ComPtr<IMFMediaSource> src;
  ComPtr<IMFSourceReader> reader;
  Converter conv;
  HRESULT hr = S_OK;
  std::string setupErr = [&]() -> std::string {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 2);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
    a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, id.c_str());
    hr = MFCreateDeviceSource(a.Get(), &src);
    if (FAILED(hr)) {
      if (hr == E_ACCESSDENIED)
        return "Windows blocked microphone access. Turn on Settings > Privacy & security > Microphone > \"Let desktop "
               "apps access your microphone\".";
      return "Couldn't open the audio input (" + HrStr(hr) + ").";
    }
    {
      std::lock_guard<std::mutex> lk(srcMutex_);
      source_ = src;
    }
    hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &reader);
    if (FAILED(hr)) return "Couldn't read the audio input (" + HrStr(hr) + ").";
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(AUD, TRUE);

    // Try to get 16-bit stereo 48 kHz straight from the device; otherwise convert ourselves.
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    want->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    want->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
    want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(reader->SetCurrentMediaType(AUD, nullptr, want.Get()))) Log("Audio: device kept its native format");

    ComPtr<IMFMediaType> curT;
    hr = reader->GetCurrentMediaType(AUD, &curT);
    if (FAILED(hr)) return "Couldn't read the audio format (" + HrStr(hr) + ").";
    WAVEFORMATEX* wf = nullptr;
    UINT32 wfSize = 0;
    hr = MFCreateWaveFormatExFromMFMediaType(curT.Get(), &wf, &wfSize);
    if (FAILED(hr) || !wf) return "Unsupported audio format (" + HrStr(hr) + ").";
    conv.Setup(wf);
    char b[96];
    snprintf(b, sizeof(b), "%.1f kHz · %s · %d-bit%s", wf->nSamplesPerSec / 1000.0, wf->nChannels == 1 ? "Mono" : "Stereo",
             (int)wf->wBitsPerSample, conv.isFloat ? " float" : "");
    formatDesc = b;
    Log("Audio opened: %s (tag %u)", b, wf->wFormatTag);
    CoTaskMemFree(wf);
    return {};
  }();

  if (!setupErr.empty()) {
    Log("Audio open failed: %s", setupErr.c_str());
    if (src) src->Shutdown();
    {
      std::lock_guard<std::mutex> lk(srcMutex_);
      source_.Reset();
    }
    finish(setupErr);
    CoUninitialize();
    return;
  }
  finish({});

  std::vector<float> out;
  while (!stop_) {
    DWORD si = 0, flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> s;
    hr = reader->ReadSample(AUD, 0, &si, &flags, &ts, &s);
    if (stop_) break;
    if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
      Log("Audio ReadSample stopped: hr=%s flags=0x%x", HrStr(hr).c_str(), flags);
      lost_ = true;
      break;
    }
    if (!s) continue;
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) continue;
    BYTE* p = nullptr;
    DWORD maxLen = 0, len = 0;
    if (FAILED(b->Lock(&p, &maxLen, &len))) continue;
    out.clear();
    conv.Process(p, len, out);
    b->Unlock();
    size_t frames = out.size() / 2;
    if (!frames) continue;
    float pl = 0, pr = 0;
    for (size_t i = 0; i < frames; i++) {
      pl = std::max(pl, std::fabs(out[i * 2]));
      pr = std::max(pr, std::fabs(out[i * 2 + 1]));
    }
    peakL = std::max(peakL.load(), pl);
    peakR = std::max(peakR.load(), pr);
    if (onAudio) onAudio(out.data(), frames, Now100ns());
  }
  reader.Reset();
  if (src) src->Shutdown();
  {
    std::lock_guard<std::mutex> lk(srcMutex_);
    source_.Reset();
  }
  CoUninitialize();
}
