#include "recorder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

bool Recorder::Start(const RecordConfig& c, int64_t startTime, std::string& err) {
  Stop();
  cfg = c;
  start_ = startTime;
  stop_ = false;
  dropped = 0;
  queuedVideo_ = 0;
  audioStarted_ = false;
  audioFrames_ = 0;
  audioBase_ = 0;
  encoderName.clear();
  std::promise<std::string> prom;
  auto fut = prom.get_future();
  th_ = std::thread([this, &prom] { Run(&prom); });
  err = fut.get();
  if (!err.empty()) {
    if (th_.joinable()) th_.join();
    return false;
  }
  active_ = true;
  return true;
}

void Recorder::Stop() {
  if (!th_.joinable()) return;
  active_ = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  th_.join();
}

uint64_t Recorder::Bytes() const {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(cfg.path.c_str(), GetFileExInfoStandard, &d)) return 0;
  return (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

void Recorder::PushVideo(const uint8_t* nv12, size_t size, int64_t t) {
  if (!active_) return;
  int64_t rel = t - start_;
  if (rel < 0) return;
  std::lock_guard<std::mutex> lk(m_);
  if (queuedVideo_ > 45) {  // encoder can't keep up: drop rather than eat all RAM
    dropped++;
    return;
  }
  Item it{true, rel, {}};
  if (!pool_.empty()) {
    it.data = std::move(pool_.back());
    pool_.pop_back();
  }
  it.data.assign(nv12, nv12 + size);
  q_.push_back(std::move(it));
  queuedVideo_++;
  cv_.notify_one();
}

void Recorder::PushAudio(const float* s, size_t frames, int64_t t) {
  if (!active_ || !cfg.audio || !frames) return;
  std::lock_guard<std::mutex> lk(m_);
  int64_t dur = int64_t(frames) * 10000000LL / 48000;
  int64_t actualStart = t - start_ - dur;
  int64_t predicted = audioBase_ + int64_t(audioFrames_ * 10000000ULL / 48000);
  if (!audioStarted_) {
    if (actualStart + dur <= 0) return;
    audioStarted_ = true;
    audioBase_ = std::max<int64_t>(0, actualStart);
    audioFrames_ = 0;
  } else if (std::llabs(actualStart - predicted) > 1000000) {  // >100 ms drift: re-anchor
    audioBase_ = actualStart - int64_t(audioFrames_ * 10000000ULL / 48000);
  }
  int64_t ts = audioBase_ + int64_t(audioFrames_ * 10000000ULL / 48000);
  audioFrames_ += frames;
  Item it{false, ts, {}};
  it.data.resize(frames * 4);
  int16_t* d = reinterpret_cast<int16_t*>(it.data.data());
  for (size_t i = 0; i < frames * 2; i++) {
    float v = std::clamp(s[i], -1.f, 1.f);
    d[i] = (int16_t)std::lrint(v * 32767.f);
  }
  q_.push_back(std::move(it));
  cv_.notify_one();
}

static HRESULT AddSample(IMFSinkWriter* w, DWORD stream, const std::vector<uint8_t>& data, int64_t t, int64_t dur) {
  ComPtr<IMFMediaBuffer> buf;
  HRESULT hr = MFCreateMemoryBuffer((DWORD)data.size(), &buf);
  if (FAILED(hr)) return hr;
  BYTE* p = nullptr;
  hr = buf->Lock(&p, nullptr, nullptr);
  if (FAILED(hr)) return hr;
  memcpy(p, data.data(), data.size());
  buf->Unlock();
  buf->SetCurrentLength((DWORD)data.size());
  ComPtr<IMFSample> s;
  hr = MFCreateSample(&s);
  if (FAILED(hr)) return hr;
  s->AddBuffer(buf.Get());
  s->SetSampleTime(t);
  s->SetSampleDuration(dur);
  return w->WriteSample(stream, s.Get());
}

void Recorder::Run(std::promise<std::string>* prom) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IMFSinkWriter> w;
  DWORD vs = 0, as = 0;
  HRESULT hr = S_OK;
  std::string err = [&]() -> std::string {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 4);
    a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    hr = MFCreateSinkWriterFromURL(cfg.path.c_str(), nullptr, a.Get(), &w);
    if (FAILED(hr)) return "Couldn't create the video file (" + HrStr(hr) + ").";

    ComPtr<IMFMediaType> vout, vin;
    MFCreateMediaType(&vout);
    vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vout->SetUINT32(MF_MT_AVG_BITRATE, cfg.mbps * 1000000);
    vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vout->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(vout.Get(), MF_MT_FRAME_SIZE, cfg.w, cfg.h);
    MFSetAttributeRatio(vout.Get(), MF_MT_FRAME_RATE, cfg.fpsN, cfg.fpsD);
    MFSetAttributeRatio(vout.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    vout->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    vout->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    vout->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    hr = w->AddStream(vout.Get(), &vs);
    if (FAILED(hr)) return "The H.264 encoder rejected " + std::to_string(cfg.w) + "x" + std::to_string(cfg.h) + " (" + HrStr(hr) + ").";

    MFCreateMediaType(&vin);
    vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vin.Get(), MF_MT_FRAME_SIZE, cfg.w, cfg.h);
    MFSetAttributeRatio(vin.Get(), MF_MT_FRAME_RATE, cfg.fpsN, cfg.fpsD);
    MFSetAttributeRatio(vin.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    vin->SetUINT32(MF_MT_DEFAULT_STRIDE, cfg.w);
    vin->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    vin->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    hr = w->SetInputMediaType(vs, vin.Get(), nullptr);
    if (FAILED(hr)) return "The H.264 encoder rejected the video input (" + HrStr(hr) + ").";

    if (cfg.audio) {
      ComPtr<IMFMediaType> aout, ain;
      MFCreateMediaType(&aout);
      aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
      aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
      aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
      aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
      aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
      hr = w->AddStream(aout.Get(), &as);
      if (FAILED(hr)) return "The AAC encoder failed (" + HrStr(hr) + ").";
      MFCreateMediaType(&ain);
      ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
      ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
      ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
      ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
      ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
      ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
      hr = w->SetInputMediaType(as, ain.Get(), nullptr);
      if (FAILED(hr)) return "The AAC encoder rejected the audio input (" + HrStr(hr) + ").";
    }
    hr = w->BeginWriting();
    if (FAILED(hr)) return "Couldn't start the encoder (" + HrStr(hr) + ").";

    // Which encoder did Windows pick? (Useful to confirm the AMD hardware encoder is in use.)
    ComPtr<IMFSinkWriterEx> wx;
    if (SUCCEEDED(w.As(&wx))) {
      for (DWORD i = 0;; i++) {
        GUID cat{};
        ComPtr<IMFTransform> t;
        if (FAILED(wx->GetTransformForStream(vs, i, &cat, &t))) break;
        if (cat != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<IMFAttributes> ta;
        WCHAR* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(t->GetAttributes(&ta)) && ta && SUCCEEDED(ta->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
          encoderName = W2U(name);
          CoTaskMemFree(name);
        }
        if (encoderName.empty()) encoderName = "H.264 encoder";
      }
    }
    if (encoderName.empty()) encoderName = "H.264";
    Log("Recording %ls: %ux%u %u/%u fps %u Mbps, encoder: %s", cfg.path.c_str(), cfg.w, cfg.h, cfg.fpsN, cfg.fpsD,
        cfg.mbps, encoderName.c_str());
    return {};
  }();
  prom->set_value(err);
  if (!err.empty()) {
    Log("Recorder start failed: %s", err.c_str());
    CoUninitialize();
    return;
  }

  const int64_t frameDur = int64_t(10000000.0 * cfg.fpsD / std::max<UINT32>(1, cfg.fpsN));
  int64_t lastVideo = -1;
  for (;;) {
    Item it;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
      if (q_.empty()) break;  // stop requested and drained
      it = std::move(q_.front());
      q_.pop_front();
      if (it.video) queuedVideo_--;
    }
    if (it.video) {
      int64_t t = it.t;
      if (t <= lastVideo) t = lastVideo + 1;
      lastVideo = t;
      hr = AddSample(w.Get(), vs, it.data, t, frameDur);
      std::lock_guard<std::mutex> lk(m_);
      if (pool_.size() < 4) pool_.push_back(std::move(it.data));
    } else {
      hr = AddSample(w.Get(), as, it.data, it.t, int64_t(it.data.size() / 4) * 10000000LL / 48000);
    }
    if (FAILED(hr)) Log("WriteSample failed: %s", HrStr(hr).c_str());
  }
  hr = w->Finalize();
  Log("Recording finalised: %s, dropped %u frames", HrStr(hr).c_str(), dropped.load());
  w.Reset();
  CoUninitialize();
}
