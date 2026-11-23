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
// Async source reader callback

class ReaderCallback : public IMFSourceReaderCallback {
 public:
  explicit ReaderCallback(std::function<void(HRESULT, DWORD, IMFSample*)> f) : fn_(std::move(f)) {
    flushed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
      *ppv = static_cast<IMFSourceReaderCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --ref_;
    if (!r) delete this;
    return r;
  }
  STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample* s) override {
    std::lock_guard<std::recursive_mutex> lk(m_);
    if (fn_) fn_(hr, flags, s);
    return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override {
    SetEvent(flushed_);
    return S_OK;
  }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

  // Stop delivering callbacks; waits for one that's running to finish.
  void Detach() {
    std::lock_guard<std::recursive_mutex> lk(m_);
    fn_ = nullptr;
  }
  HANDLE Flushed() const { return flushed_; }

 private:
  ~ReaderCallback() { CloseHandle(flushed_); }
  std::atomic<ULONG> ref_{1};
  std::recursive_mutex m_;
  std::function<void(HRESULT, DWORD, IMFSample*)> fn_;
  HANDLE flushed_;
};

// Cancels outstanding reads and tears a reader down without ever blocking for long.
static void ShutdownReader(ComPtr<IMFSourceReader>& reader, ComPtr<IMFMediaSource>& src, ReaderCallback*& cb, DWORD stream) {
  if (reader && cb) {
    ResetEvent(cb->Flushed());
    if (SUCCEEDED(reader->Flush(stream))) WaitForSingleObject(cb->Flushed(), 1500);
  }
  if (cb) cb->Detach();
  reader.Reset();
  if (src) src->Shutdown();
  src.Reset();
  if (cb) {
    cb->Release();
    cb = nullptr;
  }
}

static ComPtr<IMFAttributes> AsyncReaderAttrs(ReaderCallback* cb, bool video) {
  ComPtr<IMFAttributes> ra;
  MFCreateAttributes(&ra, 3);
  ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);
  if (video) {
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
  }
  return ra;
}

// ---------------------------------------------------------------------------------------------------------------
// Video

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

void VideoCapture::Close() {
  stop_ = true;
  ShutdownReader(reader_, src_, cb_, VID);
  open_ = false;
  std::lock_guard<std::mutex> lk(qm_);
  q_.clear();
}

bool VideoCapture::Open(const CaptureRequest& req, std::string& err) {
  Close();
  stop_ = false;
  lost_ = false;
  HRESULT hr = S_OK;
  cb_ = new ReaderCallback([this](HRESULT h, DWORD f, IMFSample* s) { OnSample(h, f, s); });
  err = [&]() -> std::string {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 2);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, req.id.c_str());
    hr = MFCreateDeviceSource(a.Get(), &src_);
    if (FAILED(hr)) return CaptureError(hr);
    hr = MFCreateSourceReaderFromMediaSource(src_.Get(), AsyncReaderAttrs(cb_, true).Get(), &reader_);
    if (FAILED(hr)) return CaptureError(hr);
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(VID, TRUE);

    std::vector<std::pair<VideoMode, DWORD>> all;
    for (DWORD i = 0;; i++) {
      ComPtr<IMFMediaType> t;
      if (FAILED(reader_->GetNativeMediaType(VID, i, &t))) break;
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
      int fmt = req.fmtPref == 2 ? (m.compressed() ? 0 : 1) : (m.compressed() ? 1 : 0);
      if (req.fmtPref == 1 && anyUncompressed && m.compressed()) resDiff += 1000;
      int sub = (m.subtype == MFVideoFormat_NV12 || m.subtype == MFVideoFormat_YUY2) ? 0 : 1;
      return std::tuple<double, double, int, int>(resDiff, fpsDiff, fmt, sub);
    };
    auto best = std::min_element(all.begin(), all.end(), [&](auto& a1, auto& b1) { return key(a1.first) < key(b1.first); });
    cur = best->first;
    ComPtr<IMFMediaType> native;
    reader_->GetNativeMediaType(VID, best->second, &native);
    hr = reader_->SetCurrentMediaType(VID, nullptr, native.Get());
    if (FAILED(hr)) return "Couldn't select " + cur.fmtName() + " mode (" + HrStr(hr) + ").";

    UINT32 nr = 0;
    nominalRange = 0;
    if (SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nr)))
      nominalRange = nr == MFNominalRange_0_255 ? 1 : nr == MFNominalRange_16_235 ? 2 : 0;

    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, cur.w, cur.h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, cur.fpsN, cur.fpsD);
    hr = reader_->SetCurrentMediaType(VID, nullptr, out.Get());
    if (FAILED(hr)) return "Couldn't convert " + cur.fmtName() + " video (" + HrStr(hr) + ").";
    ComPtr<IMFMediaType> got;
    reader_->GetCurrentMediaType(VID, &got);
    MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &outW, &outH);
    if (nominalRange == 0 && SUCCEEDED(got->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nr)))
      nominalRange = nr == MFNominalRange_0_255 ? 1 : nr == MFNominalRange_16_235 ? 2 : 0;
    Log("Video opened: %ux%u @ %.2f %s -> NV12 %ux%u, range=%d", cur.w, cur.h, cur.fps(), cur.fmtName().c_str(), outW,
        outH, nominalRange);
    hr = reader_->ReadSample(VID, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) return CaptureError(hr);
    return {};
  }();
  if (!err.empty()) {
    Log("Video open failed: %s", err.c_str());
    stop_ = true;
    ShutdownReader(reader_, src_, cb_, VID);
    return false;
  }
  open_ = true;
  return true;
}

void VideoCapture::OnSample(HRESULT hr, DWORD flags, IMFSample* s) {
  if (stop_) return;
  if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
    Log("Video read stopped: hr=%s flags=0x%x", HrStr(hr).c_str(), flags);
    lost_ = true;
    return;
  }
  if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
    ComPtr<IMFMediaType> got;
    if (SUCCEEDED(reader_->GetCurrentMediaType(VID, &got))) MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &outW, &outH);
    Log("Video size changed to %ux%u", outW, outH);
  }
  if (s) {
    UINT32 W = outW, H = outH;
    ComPtr<IMFMediaBuffer> b;
    if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
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
      bool locked = locked2d;
      if (locked2d) {
        b2->GetContiguousLength(&len);
      } else {
        DWORD maxLen = 0;
        locked = SUCCEEDED(b->Lock(&p, &maxLen, &len));
        pitch = (LONG)W;
      }
      if (locked) {
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
        if (ok) {
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
      }
    }
  }
  if (!stop_ && reader_) {
    HRESULT h2 = reader_->ReadSample(VID, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(h2)) {
      Log("Video ReadSample request failed: %s", HrStr(h2).c_str());
      lost_ = true;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Audio

// Converts any PCM / float layout to interleaved stereo float, then resamples linearly to 48 kHz.
struct AudioCapture::ConverterState {
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

AudioCapture::AudioCapture() = default;
AudioCapture::~AudioCapture() { Close(); }

void AudioCapture::Close() {
  stop_ = true;
  ShutdownReader(reader_, src_, cb_, AUD);
  open_ = false;
  peakL = 0;
  peakR = 0;
}

bool AudioCapture::Open(const std::wstring& id, std::string& err) {
  Close();
  stop_ = false;
  lost_ = false;
  conv_ = std::make_unique<ConverterState>();
  HRESULT hr = S_OK;
  cb_ = new ReaderCallback([this](HRESULT h, DWORD f, IMFSample* s) { OnSample(h, f, s); });
  err = [&]() -> std::string {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 2);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
    a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, id.c_str());
    hr = MFCreateDeviceSource(a.Get(), &src_);
    if (FAILED(hr)) {
      if (hr == E_ACCESSDENIED)
        return "Windows blocked microphone access. Turn on Settings > Privacy & security > Microphone > \"Let desktop "
               "apps access your microphone\".";
      return "Couldn't open the audio input (" + HrStr(hr) + ").";
    }
    hr = MFCreateSourceReaderFromMediaSource(src_.Get(), AsyncReaderAttrs(cb_, false).Get(), &reader_);
    if (FAILED(hr)) return "Couldn't read the audio input (" + HrStr(hr) + ").";
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(AUD, TRUE);

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
    if (FAILED(reader_->SetCurrentMediaType(AUD, nullptr, want.Get()))) Log("Audio: device kept its native format");

    ComPtr<IMFMediaType> curT;
    hr = reader_->GetCurrentMediaType(AUD, &curT);
    if (FAILED(hr)) return "Couldn't read the audio format (" + HrStr(hr) + ").";
    WAVEFORMATEX* wf = nullptr;
    UINT32 wfSize = 0;
    hr = MFCreateWaveFormatExFromMFMediaType(curT.Get(), &wf, &wfSize);
    if (FAILED(hr) || !wf) return "Unsupported audio format (" + HrStr(hr) + ").";
    conv_->Setup(wf);
    char b[96];
    snprintf(b, sizeof(b), "%.1f kHz · %s · %d-bit%s", wf->nSamplesPerSec / 1000.0, wf->nChannels == 1 ? "Mono" : "Stereo",
             (int)wf->wBitsPerSample, conv_->isFloat ? " float" : "");
    formatDesc = b;
    Log("Audio opened: %s (tag %u)", b, wf->wFormatTag);
    CoTaskMemFree(wf);
    hr = reader_->ReadSample(AUD, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) return "Couldn't start the audio input (" + HrStr(hr) + ").";
    return {};
  }();
  if (!err.empty()) {
    Log("Audio open failed: %s", err.c_str());
    stop_ = true;
    ShutdownReader(reader_, src_, cb_, AUD);
    return false;
  }
  open_ = true;
  return true;
}

void AudioCapture::OnSample(HRESULT hr, DWORD flags, IMFSample* s) {
  if (stop_) return;
  if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
    Log("Audio read stopped: hr=%s flags=0x%x", HrStr(hr).c_str(), flags);
    lost_ = true;
    return;
  }
  ComPtr<IMFMediaBuffer> b;
  if (s && SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
    BYTE* p = nullptr;
    DWORD maxLen = 0, len = 0;
    if (SUCCEEDED(b->Lock(&p, &maxLen, &len))) {
      out_.clear();
      conv_->Process(p, len, out_);
      b->Unlock();
      size_t frames = out_.size() / 2;
      if (frames) {
        float pl = 0, pr = 0;
        for (size_t i = 0; i < frames; i++) {
          pl = std::max(pl, std::fabs(out_[i * 2]));
          pr = std::max(pr, std::fabs(out_[i * 2 + 1]));
        }
        peakL = std::max(peakL.load(), pl);
        peakR = std::max(peakR.load(), pr);
        if (onAudio) onAudio(out_.data(), frames, Now100ns());
      }
    }
  }
  if (!stop_ && reader_) {
    HRESULT h2 = reader_->ReadSample(AUD, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(h2)) lost_ = true;
  }
}
