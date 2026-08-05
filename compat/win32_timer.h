// compat/win32_timer.h -- CreateWaitableTimerEx -> timerfd + pthread shim
#pragma once
#ifndef _WIN32
#include <sys/timerfd.h>
#include <sys/select.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
constexpr int CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0;
constexpr unsigned long INFINITE = 0xFFFFFFFF;
union LARGE_INTEGER {
    struct { unsigned long LowPart; long HighPart; };
    long long QuadPart;
};
inline void* CreateWaitableTimerExW(void*,void*,int,int) {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    return reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}
inline int SetWaitableTimerEx(void* h, const LARGE_INTEGER* due,
                               long period_ms, void*,void*,void*,unsigned long) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(h));
    long long due_ns = (due && due->QuadPart < 0) ? (-due->QuadPart * 100LL)
                                                    : (period_ms * 1000000LL);
    long period_ns = period_ms * 1000000L;
    struct itimerspec ts{};
    ts.it_value.tv_sec    = due_ns / 1000000000LL;
    ts.it_value.tv_nsec   = due_ns % 1000000000LL;
    ts.it_interval.tv_sec = period_ns / 1000000000L;
    ts.it_interval.tv_nsec= period_ns % 1000000000L;
    return (::timerfd_settime(fd, 0, &ts, nullptr) == 0) ? 1 : 0;
}
inline unsigned long WaitForSingleObject(void* h, unsigned long timeout_ms) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(h));
    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
    struct timeval tv{}, *tvp = nullptr;
    if (timeout_ms != 0xFFFFFFFF) {
        tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000; tvp=&tv;
    }
    int r = ::select(fd+1, &rfds, nullptr, nullptr, tvp);
    if (r > 0) { uint64_t e; ::read(fd,&e,8); return 0; }
    return (r == 0) ? 0x00000102UL : 0xFFFFFFFFUL;
}
inline int CloseHandle(void* h) {
    return (::close(static_cast<int>(reinterpret_cast<intptr_t>(h)))==0)?1:0;
}
inline int timeBeginPeriod(unsigned int) { return 0; }
inline int timeEndPeriod(unsigned int)   { return 0; }
#ifndef __stdcall
#define __stdcall
#endif
struct _bte_args { void*(*fn)(void*); void* arg; };
inline void* _bte_run(void* p) {
    auto* a=static_cast<_bte_args*>(p); auto f=a->fn; auto g=a->arg;
    delete a; return f(g);
}
inline uintptr_t _beginthreadex(void*,unsigned,
    unsigned(__stdcall* fn)(void*), void* arg, unsigned, unsigned*) {
    pthread_t t;
    auto* a=new _bte_args{reinterpret_cast<void*(*)(void*)>(fn), arg};
    if (pthread_create(&t, nullptr, _bte_run, a)!=0){delete a;return 0;}
    pthread_detach(t); return static_cast<uintptr_t>(t);
}
#endif // !_WIN32
