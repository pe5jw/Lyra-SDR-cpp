// compat/win32_dynload.h -- LoadLibrary/GetProcAddress -> dlopen/dlsym
#pragma once
#ifndef _WIN32
#include <dlfcn.h>
using HMODULE = void*;
using FARPROC = void(*)();
inline HMODULE LoadLibraryExW(const wchar_t* p, void*, unsigned long) {
    char buf[4096]{}; std::wcstombs(buf, p, sizeof(buf)-1);
    return ::dlopen(buf, RTLD_LAZY | RTLD_LOCAL);
}
inline HMODULE LoadLibraryW(const wchar_t* p) { return LoadLibraryExW(p,nullptr,0); }
inline FARPROC GetProcAddress(HMODULE h, const char* n) {
    return reinterpret_cast<FARPROC>(::dlsym(h, n));
}
inline int FreeLibrary(HMODULE h) { return (::dlclose(h) == 0) ? 1 : 0; }
using DLL_DIRECTORY_COOKIE = void*;
inline DLL_DIRECTORY_COOKIE AddDllDirectory(const wchar_t*) { return nullptr; }
inline int RemoveDllDirectory(DLL_DIRECTORY_COOKIE) { return 1; }
constexpr unsigned long LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x1000;
#endif // !_WIN32
