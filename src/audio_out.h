#pragma once
#include "common.h"

// Plays the capture card's audio through the default Windows output device (low latency, WASAPI shared mode).
class AudioMonitor {
 public:
  ~AudioMonitor() { Stop(); }
  void Start();
  void Stop();
  void Push(const float* stereo, size_t frames);  // 48 kHz interleaved stereo
  std::atomic<float> volume{0.8f};                 // 0..1 (already includes mute/monitor switch)

 private:
  void Run();
  std::thread th_;
  std::atomic<bool> stop_{false};
  std::mutex m_;
  std::deque<float> ring_;
};
