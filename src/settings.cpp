#include "settings.h"

static std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

static int GetI(const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"app", k, def, IniPath().c_str()); }
static std::wstring GetS(const wchar_t* k, const std::wstring& def) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(L"app", k, def.c_str(), buf, 2048, IniPath().c_str());
  return buf;
}
static void PutI(const wchar_t* k, int v) {
  WritePrivateProfileStringW(L"app", k, std::to_wstring(v).c_str(), IniPath().c_str());
}
static void PutS(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"app", k, v.c_str(), IniPath().c_str()); }

void Settings::Load() {
  videoId = GetS(L"videoId", L"");
  audioId = GetS(L"audioId", L"");
  audioAuto = GetI(L"audioAuto", 1) != 0;
  width = GetI(L"width", 1920);
  height = GetI(L"height", 1080);
  fps = GetI(L"fps", 60);
  fmtPref = GetI(L"fmtPref", 0);
  range = GetI(L"range", 0);
  monitor = GetI(L"monitor", 1) != 0;
  volume = GetI(L"volume", 80);
  muted = GetI(L"muted", 0) != 0;
  fit = GetI(L"fit", 0);
  sidebar = GetI(L"sidebar", 1) != 0;
  enh = GetI(L"enh", 0) != 0;
  preset = GetI(L"preset", 2);
  scaler = GetI(L"scaler", 2);
  sharpen = GetI(L"sharpen", 50);
  denoise = GetI(L"denoise", 30);
  bri = GetI(L"bri", 0) - 100;  // stored offset so negatives survive GetPrivateProfileInt
  con = GetI(L"con", 100);
  sat = GetI(L"sat", 100);
  enhRecord = GetI(L"enhRecord", 1) != 0;
  outRes = GetI(L"outRes", 0);
  bitrate = GetI(L"bitrate", 20);
  saveDir = GetS(L"saveDir", DefaultSaveDir());
  if (bri < -50 || bri > 50) bri = 0;
}

void Settings::Save() const {
  PutS(L"videoId", videoId);
  PutS(L"audioId", audioId);
  PutI(L"audioAuto", audioAuto);
  PutI(L"width", width);
  PutI(L"height", height);
  PutI(L"fps", fps);
  PutI(L"fmtPref", fmtPref);
  PutI(L"range", range);
  PutI(L"monitor", monitor);
  PutI(L"volume", volume);
  PutI(L"muted", muted);
  PutI(L"fit", fit);
  PutI(L"sidebar", sidebar);
  PutI(L"enh", enh);
  PutI(L"preset", preset);
  PutI(L"scaler", scaler);
  PutI(L"sharpen", sharpen);
  PutI(L"denoise", denoise);
  PutI(L"bri", bri + 100);
  PutI(L"con", con);
  PutI(L"sat", sat);
  PutI(L"enhRecord", enhRecord);
  PutI(L"outRes", outRes);
  PutI(L"bitrate", bitrate);
  PutS(L"saveDir", saveDir);
}
