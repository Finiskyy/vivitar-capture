#include "common.h"

#include <shlobj.h>

#include <cstdarg>
#include <cwctype>

static std::mutex g_logMutex;
static FILE* g_log = nullptr;

std::wstring AppDataDir() {
  PWSTR p = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
    dir = std::wstring(p) + L"\\VivitarCapture";
    CoTaskMemFree(p);
  } else {
    dir = L".";
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring DefaultSaveDir() {
  PWSTR p = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p))) {
    dir = std::wstring(p) + L"\\Vivitar Capture";
    CoTaskMemFree(p);
  } else {
    dir = AppDataDir() + L"\\Captures";
  }
  return dir;
}

void LogInit() {
  std::wstring path = AppDataDir() + L"\\log.txt";
  std::lock_guard<std::mutex> lk(g_logMutex);
  _wfopen_s(&g_log, path.c_str(), L"w");
}

void Log(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::lock_guard<std::mutex> lk(g_logMutex);
  if (g_log) {
    fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_log);
  }
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
}

std::string W2U(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring U2W(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string HrStr(HRESULT hr) {
  char code[32];
  snprintf(code, sizeof(code), "0x%08X", (unsigned)hr);
  wchar_t* msg = nullptr;
  DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr);
  if (!n || !msg) {
    HMODULE mf = GetModuleHandleW(L"mfplat.dll");
    if (mf)
      n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, mf,
                         (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr);
  }
  std::string out = code;
  if (n && msg) {
    std::wstring m(msg);
    while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ' || m.back() == L'.')) m.pop_back();
    out += " " + W2U(m);
  }
  if (msg) LocalFree(msg);
  return out;
}

std::wstring TimestampName() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t b[64];
  swprintf_s(b, L"%04d-%02d-%02d %02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  return b;
}

int64_t Now100ns() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (c.QuadPart / freq.QuadPart) * 10000000LL + (c.QuadPart % freq.QuadPart) * 10000000LL / freq.QuadPart;
}

bool ContainsI(const std::wstring& hay, const wchar_t* needle) {
  std::wstring h = hay, n = needle;
  for (auto& c : h) c = (wchar_t)towlower(c);
  for (auto& c : n) c = (wchar_t)towlower(c);
  return h.find(n) != std::wstring::npos;
}
