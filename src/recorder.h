#pragma once
#include "common.h"

struct RecordConfig {
  std::wstring path;
  UINT32 w = 1920, h = 1080, fpsN = 60, fpsD = 1;
  UINT32 mbps = 20;
  bool audio = true;
};

// MP4 writer: H.264 (hardware encoder when available) + AAC, fed from any thread.
class Recorder {
 public:
  ~Recorder() { Stop(); }
  bool Start(const RecordConfig& cfg, int64_t startTime, std::string& err);
  void Stop();  // drains the queue and finalises the file
  bool Active() const { return active_; }

  // NV12, tightly packed w*h*3/2. `t` is Now100ns() at capture.
  void PushVideo(const uint8_t* nv12, size_t size, int64_t t);
  // Interleaved stereo float @ 48 kHz; `t` is Now100ns() when the chunk ended.
  void PushAudio(const float* stereo, size_t frames, int64_t t);

  uint64_t Bytes() const;
  std::atomic<uint32_t> dropped{0};
  std::string encoderName;
  std::string container;  // "fragmented MP4" or "MP4"
  std::string result;     // after Stop(): empty on success, otherwise what went wrong
  RecordConfig cfg;

 private:
  struct Item {
    bool video;
    int64_t t;
    std::vector<uint8_t> data;
  };
  void Run(std::promise<std::string>* prom);

  std::thread th_;
  std::atomic<bool> active_{false}, stop_{false};
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Item> q_;
  std::vector<std::vector<uint8_t>> pool_;
  int64_t start_ = 0;
  size_t queuedVideo_ = 0;
  // audio timeline
  bool audioStarted_ = false;
  int64_t audioBase_ = 0;
  uint64_t audioFrames_ = 0;
};
