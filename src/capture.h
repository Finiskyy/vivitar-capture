#pragma once
#include "common.h"

#include <guiddef.h>
#include <mfidl.h>
#include <mfreadwrite.h>

struct DeviceInfo {
  std::wstring name;
  std::wstring id;  // symbolic link (video) or endpoint id (audio)
};
std::vector<DeviceInfo> EnumDevices(bool audio);

struct VideoMode {
  GUID subtype{};
  UINT32 w = 0, h = 0, fpsN = 0, fpsD = 1;
  double fps() const { return fpsD ? double(fpsN) / fpsD : 0.0; }
  bool compressed() const;
  std::string fmtName() const;
};

// One captured frame, always NV12, tightly packed (w*h luma + w*h/2 chroma).
struct VideoFrame {
  std::vector<uint8_t> data;
  UINT32 w = 0, h = 0;
  int64_t t = 0;  // Now100ns() at arrival
  bool dup = false;  // looks the same as the previous frame (e.g. a 30 fps game captured at 60 fps)
};

struct CaptureRequest {
  std::wstring id;
  UINT32 w = 1920, h = 1080;
  double fps = 60;
  int fmtPref = 0;  // 0 auto (best quality), 1 prefer uncompressed, 2 prefer MJPEG
};

class ReaderCallback;

// Captures from a UVC device with an asynchronous Media Foundation source reader.
// Open() and Close() block briefly, so call them off the UI thread.
class VideoCapture {
 public:
  ~VideoCapture() { Close(); }
  bool Open(const CaptureRequest& req, std::string& err);
  void Close();
  bool IsOpen() const { return open_; }
  bool Lost() const { return lost_; }

  // Moves queued frames (oldest first) into `out`; give them back with Recycle().
  void TakeFrames(std::vector<VideoFrame>& out);
  void Recycle(std::vector<VideoFrame>& frames);

  void SetOnFrame(std::function<void(const VideoFrame&)> f) {
    std::lock_guard<std::mutex> lk(cbMutex_);
    onFrame_ = std::move(f);
  }

  std::vector<VideoMode> modes;  // everything the device offers
  VideoMode cur;                 // the native mode in use
  UINT32 outW = 0, outH = 0;     // NV12 output size
  int nominalRange = 0;          // 0 unknown, 1 full (0-255), 2 limited (16-235)

 private:
  void OnSample(HRESULT hr, DWORD flags, IMFSample* s);
  bool IsDuplicate(const VideoFrame& f);

  std::atomic<bool> open_{false}, stop_{false}, lost_{false};
  ComPtr<IMFMediaSource> src_;
  ComPtr<IMFSourceReader> reader_;
  ReaderCallback* cb_ = nullptr;
  std::vector<int> sig_, prevSig_;
  std::mutex qm_;
  std::deque<VideoFrame> q_;
  std::vector<VideoFrame> pool_;
  std::mutex cbMutex_;
  std::function<void(const VideoFrame&)> onFrame_;
};

// Audio from the capture card, delivered as float stereo @ 48 kHz.
class AudioCapture {
 public:
  AudioCapture();
  ~AudioCapture();
  bool Open(const std::wstring& endpointId, std::string& err);
  void Close();
  bool IsOpen() const { return open_; }
  bool Lost() const { return lost_; }

  // Called on a Media Foundation thread: interleaved stereo float, `frames` frames, arrival time of the chunk end.
  // Set before Open().
  std::function<void(const float*, size_t frames, int64_t t)> onAudio;
  std::string formatDesc;
  std::atomic<float> peakL{0}, peakR{0};

 private:
  void OnSample(HRESULT hr, DWORD flags, IMFSample* s);
  std::atomic<bool> open_{false}, stop_{false}, lost_{false};
  ComPtr<IMFMediaSource> src_;
  ComPtr<IMFSourceReader> reader_;
  ReaderCallback* cb_ = nullptr;
  struct ConverterState;
  std::unique_ptr<ConverterState> conv_;
  std::vector<float> out_;
};
