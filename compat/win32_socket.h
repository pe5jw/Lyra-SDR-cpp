// compat/win32_socket.h -- WinSock2 -> POSIX sockets shim
#pragma once
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>
using SOCKET  = int;
using DWORD   = unsigned long;
using WORD    = unsigned short;
using BOOL    = int;
using HANDLE  = void*;
using HMODULE = void*;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int    SOCKET_ERROR   = -1;
inline int closesocket(SOCKET s) { return ::close(s); }
inline int ioctlsocket(SOCKET, long, unsigned long*) { return 0; }
struct WSAData { int dummy; };
inline int WSAStartup(WORD, WSAData*) { return 0; }
inline int WSACleanup()              { return 0; }
inline int WSAGetLastError()         { return errno; }
inline DWORD FormatMessageW(DWORD, void*, DWORD code, DWORD,
                             wchar_t* buf, DWORD size, void*) {
    std::string msg = std::strerror((int)code);
    std::mbstowcs(buf, msg.c_str(), size);
    return (DWORD)msg.size();
}
struct MIB_UDPSTATS { DWORD dwInDatagrams; DWORD dwNoBufRecv; DWORD dwOutDatagrams; };
inline int GetUdpStatisticsEx(MIB_UDPSTATS* s, int) { if (s) *s = {}; return 0; }
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR
#endif
#endif // !_WIN32
