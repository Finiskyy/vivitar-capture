#pragma once
#include "common.h"

struct Settings {
  std::wstring videoId, audioId;  // empty audioId + audioAuto=false means "No audio"
  bool audioAuto = true;
  int width = 1920, height = 1080;
  int fps = 60;
  int fmtPref = 0;  // 0 auto (best quality), 1 uncompressed, 2 MJPEG
  int range = 0;    // 0 auto, 1 limited, 2 full

  bool monitor = true;
  int volume = 80;
  bool muted = false;

  int fit = 0;  // Fit enum
  bool sidebar = true;

  bool enh = false;
  int preset = 2;  // 0 fast, 1 subtle, 2 balanced, 3 strong, 4 custom
  int scaler = 2, sharpen = 50, denoise = 30, bri = 0, con = 100, sat = 100;
  bool enhRecord = true;
  int outRes = 0;  // 0 same as source, 1 1440p

  int bitrate = 20;
  std::wstring saveDir;

  void Load();
  void Save() const;
};
