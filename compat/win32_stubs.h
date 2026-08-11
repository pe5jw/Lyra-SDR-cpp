#pragma once
#ifndef _WIN32

#include <pthread.h>
#include <semaphore.h>
#include <cstdint>
#include <chrono>

// Basic Windows types
#ifndef __stdcall
#define __stdcall
#endif
#define WINAPI
#define CALLBACK
#define HWND void*
#define SW_HIDE 0
#ifndef DWORD_DEFINED
#define DWORD_DEFINED
using DWORD  = unsigned long;
using WORD   = unsigned short;
using BOOL   = int;
using HANDLE = void*;
using UINT   = unsigned int;
using LONG   = long;
using ULONG  = unsigned long;
using BYTE   = unsigned char;
using LPVOID = void*;
#endif

// HANDLE sentinel
#define INVALID_HANDLE_VALUE ((HANDLE)(uintptr_t)-1)

// Error codes
constexpr int NO_ERROR       = 0;
constexpr int WAIT_OBJECT_0  = 0;
constexpr int WAIT_TIMEOUT   = 0x00000102;
constexpr int WAIT_FAILED    = (int)0xFFFFFFFF;
constexpr DWORD STILL_ACTIVE = 259;

// FormatMessage flags
constexpr DWORD FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x00000100;
constexpr DWORD FORMAT_MESSAGE_FROM_SYSTEM     = 0x00001000;
constexpr DWORD FORMAT_MESSAGE_IGNORE_INSERTS  = 0x00000200;
#define MAKELANGID(p,s) (((DWORD)(s) << 10) | (DWORD)(p))
constexpr WORD LANG_NEUTRAL    = 0x00;
constexpr WORD SUBLANG_DEFAULT = 0x01;
inline void* LocalFree(void*) { return nullptr; }
inline DWORD GetLastError()   { return (DWORD)errno; }

// Process/thread priority
constexpr int ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000;
constexpr int HIGH_PRIORITY_CLASS         = 0x00000080;
inline int   SetPriorityClass(HANDLE, DWORD) { return 1; }
inline HANDLE GetCurrentProcess()            { return nullptr; }
inline int   CoInitializeEx(void*, DWORD)    { return 0; }
inline void  CoUninitialize()                {}

// timeGetTime
inline DWORD timeGetTime() {
    using namespace std::chrono;
    return (DWORD)(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
}

// timeBeginPeriod / timeEndPeriod
inline int timeBeginPeriod(unsigned int) { return 0; }
inline int timeEndPeriod(unsigned int)   { return 0; }

// avrt stubs
inline HANDLE AvSetMmThreadCharacteristicsA(const char*, DWORD*) { return (HANDLE)1; }
inline HANDLE AvSetMmThreadCharacteristicsW(const wchar_t*, DWORD*) { return (HANDLE)1; }
inline int    AvSetMmThreadPriority(HANDLE, int) { return 1; }
inline int    AvRevertMmThreadCharacteristics(HANDLE) { return 1; }

// SetThreadCharacteristics stub
inline HANDLE SetThreadCharacteristics(const wchar_t*, DWORD*) { return (HANDLE)1; }

// CRITICAL_SECTION -> pthread_mutex_t
typedef pthread_mutex_t CRITICAL_SECTION;
inline void InitializeCriticalSection(CRITICAL_SECTION* cs)                      { pthread_mutex_init(cs, nullptr); }
inline void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD)   { pthread_mutex_init(cs, nullptr); }
inline void DeleteCriticalSection(CRITICAL_SECTION* cs)                           { pthread_mutex_destroy(cs); }
inline void EnterCriticalSection(CRITICAL_SECTION* cs)                            { pthread_mutex_lock(cs); }
inline void LeaveCriticalSection(CRITICAL_SECTION* cs)                            { pthread_mutex_unlock(cs); }
inline int  TryEnterCriticalSection(CRITICAL_SECTION* cs)                         { return pthread_mutex_trylock(cs) == 0 ? 1 : 0; }

// Windows Semaphore -> POSIX sem_t (heap allocated so HANDLE works)
inline HANDLE CreateSemaphore(void*, long init, long, void*) {
    sem_t* s = new sem_t;
    sem_init(s, 0, (unsigned)init);
    return (HANDLE)s;
}
inline HANDLE CreateSemaphoreA(void* a, long i, long m, void* n) { return CreateSemaphore(a,i,m,n); }
inline int CloseHandle(HANDLE h) {
    if (h && h != INVALID_HANDLE_VALUE) {
        sem_t* s = (sem_t*)h;
        sem_destroy(s);
        delete s;
    }
    return 1;
}
inline int ReleaseSemaphore(HANDLE h, long, long*) {
    return sem_post((sem_t*)h) == 0 ? 1 : 0;
}
inline DWORD WaitForSingleObject(HANDLE h, DWORD timeout_ms) {
    if (timeout_ms == 0xFFFFFFFF) {
        return sem_wait((sem_t*)h) == 0 ? WAIT_OBJECT_0 : WAIT_FAILED;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r = sem_timedwait((sem_t*)h, &ts);
    return (r == 0) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
inline DWORD WaitForMultipleObjects(DWORD n, const HANDLE* h, int all, DWORD timeout_ms) {
    // Simple: wait on first handle only
    return WaitForSingleObject(h[0], timeout_ms);
}

// _beginthreadex proc type
using _beginthreadex_proc_type = unsigned(__attribute__((stdcall))*)(void*);
inline uintptr_t _beginthreadex(void*, unsigned,
    _beginthreadex_proc_type fn, void* arg, unsigned, unsigned*) {
    pthread_t tid;
    struct A { _beginthreadex_proc_type fn; void* arg; };
    auto* a = new A{fn, arg};
    pthread_create(&tid, nullptr, [](void* p) -> void* {
        auto* a = (A*)p; auto f=a->fn; auto g=a->arg; delete a; f(g); return nullptr;
    }, a);
    pthread_detach(tid);
    return (uintptr_t)tid;
}

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif



// Aligned memory (_aligned_malloc/_aligned_free -> posix_memalign/free)
#include <cstdlib>
inline void* _aligned_malloc(size_t size, size_t align) {
    void* p = nullptr;
    posix_memalign(&p, align, size);
    return p;
}
inline void _aligned_free(void* p) { free(p); }

// Interlocked intrinsics -> GCC __atomic builtins
#include <stdint.h>
inline long _InterlockedAnd(volatile long* p, long v) {
    return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST);
}
inline long _InterlockedOr(volatile long* p, long v) {
    return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);
}
inline long _InterlockedExchange(volatile long* p, long v) {
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
inline long _InterlockedCompareExchange(volatile long* p, long newv, long cmp) {
    __atomic_compare_exchange_n(p, &cmp, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
inline long _InterlockedIncrement(volatile long* p) {
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}
inline long _InterlockedDecrement(volatile long* p) {
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}

// Console/Window stubs
inline void* GetConsoleWindow()           { return nullptr; }
inline int   AllocConsole()               { return 0; }
inline int   FreeConsole()                { return 0; }
inline int   ShowWindow(void*, int)       { return 0; }
inline int   freopen_s(FILE**, const char*, const char*, FILE*) { return 0; }

#endif // !_WIN32


// Thread functions
inline void _endthread()  {}
inline void _endthreadex(unsigned) {}
inline uintptr_t _beginthread(void(*fn)(void*), unsigned, void* arg) {
    pthread_t tid;
    pthread_create(&tid, nullptr, [](void* p) -> void* {
        auto* a = (std::pair<void(*)(void*),void*>*)p;
        a->first(a->second); delete a; return nullptr;
    }, new std::pair<void(*)(void*),void*>(fn, arg));
    pthread_detach(tid);
    return (uintptr_t)tid;
}

// Sleep
inline void Sleep(unsigned long ms) {
    struct timespec ts{ (time_t)(ms/1000), (long)(ms%1000)*1000000L };
    nanosleep(&ts, nullptr);
}

// Thread priority
inline HANDLE GetCurrentThread() { return (HANDLE)pthread_self(); }
constexpr int THREAD_PRIORITY_HIGHEST      =  2;
constexpr int THREAD_PRIORITY_ABOVE_NORMAL =  1;
constexpr int THREAD_PRIORITY_NORMAL       =  0;
constexpr int THREAD_PRIORITY_LOWEST       = -2;
inline int SetThreadPriority(HANDLE, int) { return 1; }

// TEXT macro
#ifndef TEXT
#define TEXT(x) x
#endif

// AvSetMmThreadCharacteristics (non-W version)
inline HANDLE AvSetMmThreadCharacteristics(const char* n, DWORD* t) {
    return AvSetMmThreadCharacteristicsA(n, t);
}

// AVRT_PRIORITY enum
typedef int AVRT_PRIORITY;
constexpr AVRT_PRIORITY AVRT_PRIORITY_NORMAL  = 0;
constexpr AVRT_PRIORITY AVRT_PRIORITY_HIGH    = 1;
constexpr AVRT_PRIORITY AVRT_PRIORITY_CRITICAL= 2;

// InterlockedBitTestAndReset
inline unsigned char InterlockedBitTestAndReset(volatile long* p, long bit) {
    long mask = 1L << bit;
    long old = __atomic_fetch_and(p, ~mask, __ATOMIC_SEQ_CST);
    return (unsigned char)((old >> bit) & 1);
}
inline unsigned char InterlockedBitTestAndSet(volatile long* p, long bit) {
    long mask = 1L << bit;
    long old = __atomic_fetch_or(p, mask, __ATOMIC_SEQ_CST);
    return (unsigned char)((old >> bit) & 1);
}

// INFINITE timeout
constexpr unsigned long INFINITE = 0xFFFFFFFF;

// ULONG_PTR for pointer-sized integers
using ULONG_PTR = uintptr_t;
using LONG_PTR  = intptr_t;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

// Module handle stubs
inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
inline HMODULE GetModuleHandleA(const char*)    { return nullptr; }
