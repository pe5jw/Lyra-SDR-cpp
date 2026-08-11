#pragma once
#ifndef _WIN32

#include <dlfcn.h>
#include <cstring>

using HMODULE = void*;
using FARPROC = void(*)();

static inline HMODULE _lyra_dlopen_w(const wchar_t* path) {
    char buf[4096] = {};
    for (int i = 0; path[i] && i < 4095; ++i)
        buf[i] = (char)path[i];
    return ::dlopen(buf, RTLD_LAZY | RTLD_LOCAL);
}

inline HMODULE LoadLibraryExW(const wchar_t* path, void*, unsigned long) { return _lyra_dlopen_w(path); }
inline HMODULE LoadLibraryW(const wchar_t* path) { return _lyra_dlopen_w(path); }
inline FARPROC GetProcAddress(HMODULE h, const char* name) { return reinterpret_cast<FARPROC>(::dlsym(h, name)); }
inline int FreeLibrary(HMODULE h) { return (::dlclose(h) == 0) ? 1 : 0; }

using DLL_DIRECTORY_COOKIE = void*;
inline DLL_DIRECTORY_COOKIE AddDllDirectory(const wchar_t*) { return nullptr; }
inline int RemoveDllDirectory(DLL_DIRECTORY_COOKIE) { return 1; }
constexpr unsigned long LOAD_LIBRARY_SEARCH_DEFAULT_DIRS    = 0x1000;
constexpr unsigned long LOAD_LIBRARY_SEARCH_USER_DIRS       = 0x0400;
constexpr unsigned long LOAD_LIBRARY_SEARCH_APPLICATION_DIR = 0x0200;
constexpr unsigned long LOAD_LIBRARY_SEARCH_SYSTEM32        = 0x0800;

#endif
