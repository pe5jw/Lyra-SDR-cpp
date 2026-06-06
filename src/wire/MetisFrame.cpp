// Lyra — MetisFrame wire-emit primitive implementation.
// See MetisFrame.h for the §6-B + §7 reference-parity contract.

#include "wire/MetisFrame.h"

#include <array>
#include <cstring>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  using socket_send_len_t  = int;
  using socket_send_size_t = int;
#else
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_send_len_t  = ssize_t;
  using socket_send_size_t = size_t;
#endif

namespace lyra::wire {

// ---- TU-scope file-scope mutable state ----
//
// Direct mirror of the reference's:
//   `unsigned int MetisOutBoundSeqNum;`   (`networkproto1.c:30`)
//   `extern SOCKET listenSock;`           (file-scope)
//   `prn->base_outbound_port`             (held in the prn struct)
//
// Per §6-B sign-off 2026-06-06: these supersede §6 Q3
// (`Ep2SendThread::out_seq_num_` member) + §6 Q5 (the §6
// `send_lock_` mutex) — bringing Lyra's structure into direct
// alignment with the reference's TU-scope globals + free-function
// + no-lock pattern.
//
// Type:  plain `std::uint32_t` (NOT `std::atomic`).  Reference is
// `unsigned int` (also non-atomic).  Reference safety rests on
// TEMPORAL SEPARATION (priming completes before main-send spins
// up — see MetisFrame.h header).  Lyra preserves that contract.
namespace {
std::uint32_t  g_metis_out_seq_num{0};
int            g_metis_socket_fd  {-1};
const void*    g_metis_dest_addr  {nullptr};
std::size_t    g_metis_dest_addrlen{0};

// EP2 outbound datagram: 8-byte HPSDR header + 1024-byte payload
// = 1032 bytes.  Direct mirror of reference's
// `unsigned char framebuf[1032];` (`networkproto1.c:218-220`).
constexpr std::size_t kHpsdrHeaderBytes = 8;
constexpr std::size_t kPayloadBytes     = 1024;
constexpr std::size_t kDatagramBytes    = kHpsdrHeaderBytes + kPayloadBytes;
}  // namespace

// ---- metis_wire_bind ----

void metis_wire_bind(int          socket_fd,
                     const void*  dest_addr,
                     std::size_t  dest_addrlen) {
    g_metis_socket_fd   = socket_fd;
    g_metis_dest_addr   = dest_addr;
    g_metis_dest_addrlen = dest_addrlen;
}

// ---- metis_write_frame ----
//
// Reference-verbatim port of `int MetisWriteFrame(int endpoint,
// char* bufp)` at `networkproto1.c:216-237`.

int metis_write_frame(int            endpoint,
                      const uint8_t* payload_1024) {
    // Caller contract:  `metis_wire_bind()` MUST have been called
    // first with a valid socket + dest_addr, and `payload_1024`
    // MUST be a valid 1024-byte buffer.  Reference does NOT
    // null-check anything here — it would crash on a null
    // `listenSock` / `prn`.  Lyra preserves the same contract
    // (no defensive guards; the calling discipline IS the
    // safety property per "do as reference, period").

    std::array<uint8_t, kDatagramBytes> framebuf{};

    // 4-byte HPSDR sync prefix (`:223-226`).
    framebuf[0] = 0xEF;
    framebuf[1] = 0xFE;
    framebuf[2] = 0x01;
    framebuf[3] = static_cast<uint8_t>(endpoint);

    // 4-byte outbound sequence number, BE pack (`:221, 227-231`).
    // Reference uses `unsigned char* p = (unsigned char*)
    // &MetisOutBoundSeqNum; framebuf[4]=p[3]; framebuf[5]=p[2];
    // framebuf[6]=p[1]; framebuf[7]=p[0];` — a manual byte-swap
    // that relies on little-endian host layout.  Lyra uses
    // explicit shifts that are endian-portable and produce the
    // IDENTICAL 4 wire bytes (high byte first).  Post-increment
    // ordering (read → write bytes → increment) preserved
    // verbatim per Rule 24.
    const std::uint32_t seq = g_metis_out_seq_num;
    framebuf[4] = static_cast<uint8_t>((seq >> 24) & 0xff);
    framebuf[5] = static_cast<uint8_t>((seq >> 16) & 0xff);
    framebuf[6] = static_cast<uint8_t>((seq >>  8) & 0xff);
    framebuf[7] = static_cast<uint8_t>( seq        & 0xff);
    // Post-increment matches reference `++MetisOutBoundSeqNum;`
    // at `:231` — preserved verbatim per Rule 24.
    ++g_metis_out_seq_num;

    // Payload copy (`:232`).
    std::memcpy(framebuf.data() + kHpsdrHeaderBytes,
                payload_1024,
                kPayloadBytes);

    // sendto (`:234`).  Reference uses `sendPacket(listenSock,
    // ..., prn->base_outbound_port)` which wraps `sendto` with
    // the configured destination port.  Lyra reads the bound
    // socket + sockaddr from the TU-scope globals.
    const socket_send_len_t result = ::sendto(
        g_metis_socket_fd,
        reinterpret_cast<const char*>(framebuf.data()),
        static_cast<socket_send_size_t>(kDatagramBytes),
        0,
        static_cast<const sockaddr*>(g_metis_dest_addr),
        static_cast<int>(g_metis_dest_addrlen));

    if (result < 0) {
        return -1;
    }
    // Return payload bytes (matches reference `result -= 8;`
    // at `:235`).
    return static_cast<int>(result) - static_cast<int>(kHpsdrHeaderBytes);
}

// ---- diagnostic accessor ----

std::uint32_t metis_out_seq_num() {
    return g_metis_out_seq_num;
}

// ---- TU-scope socket accessor ----

int metis_socket_fd() {
    return g_metis_socket_fd;
}

}  // namespace lyra::wire
