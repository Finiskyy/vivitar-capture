#include "settings.h"

static std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

static int GetI(const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"app", k, def, IniPath().c_str()); }
static std::wstring GetS(const wchar_t* k, const std::wstring& def) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(L"app", k, def.c_str(), buf, 2048, IniPath().c_str());
  return buf;
}

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
  tnr = GetI(L"tnr", 40);
  bri = GetI(L"bri", 0) - 100;  // stored offset so negatives survive GetPrivateProfileInt
  con = GetI(L"con", 100);
  sat = GetI(L"sat", 100);
  enhRecord = GetI(L"enhRecord", 1) != 0;
  outRes = GetI(L"outRes", 0);
  bitrate = GetI(L"bitrate", 20);
  frameGen = GetI(L"frameGen", 0) != 0;
  dedupe = GetI(L"dedupe", 1) != 0;
  upscaler = GetI(L"upscaler", 0);
  saveDir = GetS(L"saveDir", DefaultSaveDir());
  if (bri < -50 || bri > 50) bri = 0;
}

void Settings::Save() const {
  // Build the whole file and write it once (WritePrivateProfileString per key is slow and caused hitches).
  std::wstring t = L"[app]\r\n";
  auto i = [&](const wchar_t* k, int v) { t += std::wstring(k) + L"=" + std::to_wstring(v) + L"\r\n"; };
  auto str = [&](const wchar_t* k, const std::wstring& v) { t += std::wstring(k) + L"=" + v + L"\r\n"; };
  str(L"videoId", videoId);
  str(L"audioId", audioId);
  i(L"audioAuto", audioAuto);
  i(L"width", width);
  i(L"height", height);
  i(L"fps", fps);
  i(L"fmtPref", fmtPref);
  i(L"range", range);
  i(L"monitor", monitor);
  i(L"volume", volume);
  i(L"muted", muted);
  i(L"fit", fit);
  i(L"sidebar", sidebar);
  i(L"enh", enh);
  i(L"preset", preset);
  i(L"scaler", scaler);
  i(L"sharpen", sharpen);
  i(L"denoise", denoise);
  i(L"tnr", tnr);
  i(L"bri", bri + 100);
  i(L"con", con);
  i(L"sat", sat);
  i(L"enhRecord", enhRecord);
  i(L"outRes", outRes);
  i(L"bitrate", bitrate);
  i(L"frameGen", frameGen);
  i(L"dedupe", dedupe);
  i(L"upscaler", upscaler);
  str(L"saveDir", saveDir);
  std::wstring path = IniPath(), tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  const wchar_t bom = 0xFEFF;  // UTF-16LE so device names/paths with any characters survive
  DWORD wr = 0;
  WriteFile(h, &bom, 2, &wr, nullptr);
  WriteFile(h, t.data(), DWORD(t.size() * sizeof(wchar_t)), &wr, nullptr);
  CloseHandle(h);
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
