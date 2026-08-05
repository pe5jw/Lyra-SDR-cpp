#pragma once
#ifndef _WIN32

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#ifndef DWORD_DEFINED
#define DWORD_DEFINED
using SOCKET  = int;
using DWORD   = unsigned long;
using WORD    = unsigned short;
using BOOL    = int;
using HANDLE  = void*;
using HMODULE = void*;
#endif

constexpr SOCKET INVALID_SOCKET = -1;
constexpr int    SOCKET_ERROR   = -1;

inline int closesocket(SOCKET s)  { return ::close(s); }
inline int ioctlsocket(SOCKET, long, unsigned long*) { return 0; }

struct WSAData { int dummy; };
inline int WSAStartup(WORD, WSAData*) { return 0; }
inline int WSACleanup()              { return 0; }
inline int WSAGetLastError()         { return errno; }

inline DWORD FormatMessageW(DWORD, void*, DWORD, DWORD, void*, DWORD, void*) { return 0; }

struct MIB_UDPSTATS { DWORD dwInDatagrams; DWORD dwNoBufRecv; DWORD dwOutDatagrams; DWORD dwNoPorts; DWORD dwInErrors; };
inline int GetUdpStatisticsEx(MIB_UDPSTATS* s, int) {
    if (s) { s->dwInDatagrams = 0; s->dwNoBufRecv = 0; s->dwOutDatagrams = 0; }
    return 0;
}

#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR
#endif

#endif
