// compat/win32_stubs.h -- misc Windows stubs for Linux/macOS
#pragma once
#ifndef _WIN32
#include <cstdint>
#include <chrono>
using UINT = unsigned int; using LONG = long; using ULONG = unsigned long;
constexpr int ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000;
constexpr int HIGH_PRIORITY_CLASS         = 0x00000080;
inline int  SetPriorityClass(void*, unsigned long) { return 1; }
inline void* GetCurrentProcess()                   { return nullptr; }
inline int  CoInitializeEx(void*, unsigned long)   { return 0; }
inline void CoUninitialize() {}
inline unsigned long timeGetTime() {
    using namespace std::chrono;
    return (unsigned long)(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
}
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif // !_WIN32
