#include "audio_out.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>

void AudioMonitor::Start() {
  if (th_.joinable()) return;
  stop_ = false;
  th_ = std::thread([this] { Run(); });
}

void AudioMonitor::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  std::lock_guard<std::mutex> lk(m_);
  ring_.clear();
}

void AudioMonitor::Push(const float* s, size_t frames) {
  if (!th_.joinable()) return;
  std::lock_guard<std::mutex> lk(m_);
  ring_.insert(ring_.end(), s, s + frames * 2);
  // Keep latency low: if more than 100 ms has built up, drop down to 40 ms.
  if (ring_.size() > 4800 * 2) ring_.erase(ring_.begin(), ring_.end() - 1920 * 2);
}

void AudioMonitor::Run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IMMDeviceEnumerator> en;
  ComPtr<IMMDevice> dev;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
  if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
  WAVEFORMATEXTENSIBLE fmt{};
  fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  fmt.Format.nChannels = 2;
  fmt.Format.nSamplesPerSec = 48000;
  fmt.Format.wBitsPerSample = 32;
  fmt.Format.nBlockAlign = 8;
  fmt.Format.nAvgBytesPerSec = 48000 * 8;
  fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  fmt.Samples.wValidBitsPerSample = 32;
  fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  if (SUCCEEDED(hr))
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            200000 /* 20 ms */, 0, &fmt.Format, nullptr);
  UINT32 bufFrames = 0;
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(evt);
  if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufFrames);
  if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    Log("Audio monitor failed to start: %s", HrStr(hr).c_str());
    CloseHandle(evt);
    CoUninitialize();
    return;
  }
  Log("Audio monitor started (%u frame buffer)", bufFrames);
  while (!stop_) {
    WaitForSingleObject(evt, 100);
    UINT32 pad = 0;
    if (FAILED(client->GetCurrentPadding(&pad))) break;
    UINT32 avail = bufFrames - pad;
    if (!avail) continue;
    BYTE* data = nullptr;
    if (FAILED(render->GetBuffer(avail, &data))) break;
    float* out = reinterpret_cast<float*>(data);
    float vol = volume.load();
    size_t have = 0;
    {
      std::lock_guard<std::mutex> lk(m_);
      have = std::min<size_t>(avail, ring_.size() / 2);
      for (size_t i = 0; i < have * 2; i++) out[i] = ring_[i] * vol;
      ring_.erase(ring_.begin(), ring_.begin() + have * 2);
    }
    for (size_t i = have * 2; i < size_t(avail) * 2; i++) out[i] = 0.f;
    render->ReleaseBuffer(avail, 0);
  }
  client->Stop();
  CloseHandle(evt);
  CoUninitialize();
}
