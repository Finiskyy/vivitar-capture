#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// Logging goes to %APPDATA%\VivitarCapture\log.txt so problems on the user's PC can be diagnosed.
void LogInit();
void Log(const char* fmt, ...);

std::string W2U(const std::wstring& w);
std::wstring U2W(const std::string& s);
std::string HrStr(HRESULT hr);
std::wstring AppDataDir();
std::wstring DefaultSaveDir();
std::wstring TimestampName();  // "2026-11-23 19-20-05"

// QueryPerformanceCounter in 100 ns units (the Media Foundation time base).
int64_t Now100ns();

// Lower-case ASCII compare helper for device-name matching.
bool ContainsI(const std::wstring& hay, const wchar_t* needle);
