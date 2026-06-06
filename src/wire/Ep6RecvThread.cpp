// Lyra — EP6 receive thread implementation.
//
// See Ep6RecvThread.h for the design commentary + source mirror.
// This file mirrors `ChannelMaster/networkproto1.c:422-586` (the
// `MetisReadThreadMainLoop_HL2` function) verbatim per the signed
// §5 parity checkpoint, the §5-A parity-correction audit
// (2026-06-05), and the §1-C Stage 4B parity-correction sweep
// (2026-06-06).  Reference defects (dash_in/dot_in left-shift
// always-zero; HL2 single-frame adc_overload assignment vs the
// generic OR-until-cleared) are PRESERVED verbatim per Rule 24.
//
// §1-C Stage 4B: buffers (`RxBuff` / `TxReadBufp` / `ReadBufp`)
// migrated from class members to `prn->...` (per §1.1 revert);
// `ControlBytesIn[5]` / `MetisLastRecvSeq` / `SeqError` migrated
// from class members to TU-scope statics here (per reference,
// these are file-scope globals NOT in `_radionet`); the recv()
// loop replaced with `WSAEventSelect` + `WSAWaitForMultipleEvents`
// (per `:433-462` reference verbatim, using `prn->hDataEvent` +
// `prn->wsaProcessEvents` per §1.1 revert).

#include "wire/Ep6RecvThread.h"
#include "wire/RadioNet.h"
#include "wire/Router.h"

#include <cstring>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <avrt.h>
  #pragma comment(lib, "avrt.lib")
  using socket_recv_len_t  = int;
  using socket_recv_size_t = int;
#else
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_recv_len_t  = ssize_t;
  using socket_recv_size_t = size_t;
#endif

namespace lyra::wire {

// ---- constants mirroring the reference layout ----
namespace {
// One UDP datagram = 8-byte HPSDR header (4 sync 0xEF/0xFE/0x01/
// 0x06 + 4-byte BE sequence) + 2 x 512-byte USB frames.
constexpr std::size_t kHpsdrHeaderBytes = 8;
constexpr std::size_t kUsbFrameBytes    = 512;
constexpr std::size_t kEp6DatagramBytes =
    kHpsdrHeaderBytes + 2 * kUsbFrameBytes;  // 1032

// USB frame layout: 3-byte sync + 5-byte C0..C4 status + 504
// bytes of sample slots.  The reference's per-sample-slot index
// uses `k = 8 + isample * (6*nddc + 2) + iddc * 6` — that is,
// the sample area begins at byte 8 within the 512-byte USB frame
// (3 sync + 5 CC = 8).
constexpr int kFrameSampleAreaOffset = 8;
constexpr int kSampleAreaBytes       = 504;

// Reference IQ-pack divisor.  Per `networkproto1.c:533-540` each
// 24-bit BE IQ value is packed into the TOP 24 bits of an int32
// (`bptr[k+0] << 24 | bptr[k+1] << 16 | bptr[k+2] << 8`, low 8
// bits zero) and then multiplied by `1.0 / 2147483648.0`.  The
// effective normalization is `int24_value / 2^23` → ±1.0 double.
constexpr double kIqDivisor = 1.0 / 2147483648.0;

// Per-DDC stride within a sample slot: 3 bytes I + 3 bytes Q.
constexpr int kBytesPerDdc = 6;
// Mic trailer within a sample slot: 2 bytes (16-bit BE).
constexpr int kMicTrailerBytes = 2;

// Unpack one 24-bit BE IQ word from `p[0..2]` into a normalized
// double, exactly mirroring the reference's bit pattern (top-24
// pack + 1/2^31 divisor → effective /2^23).  Done via uint32
// build + signed conversion to keep the path defined under C++23
// (the reference's direct `unsigned char << 24` into a signed
// int is technically UB but works on every two's-complement
// platform; the uint32 build below is byte-identical and well-
// defined).
inline double unpack_iq_be(const uint8_t* p) {
    const uint32_t raw = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16) |
                         (static_cast<uint32_t>(p[2]) <<  8);
    return static_cast<double>(static_cast<int32_t>(raw)) * kIqDivisor;
}

// Unpack one 16-bit BE mic word from `p[0..1]` into a normalized
// double via the SAME divisor — reference packs `bptr[k+0] << 24
// | bptr[k+1] << 16` (top 16 bits of int32) and scales by
// `1/2^31` → effective /2^15.  Result is in ±1.0.
inline double unpack_mic_be(const uint8_t* p) {
    const uint32_t raw = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16);
    return static_cast<double>(static_cast<int32_t>(raw)) * kIqDivisor;
}

// ---- TU-scope file-scope mirrors (§1-C Stage 4B) ----
//
// Reference puts these at FILE SCOPE in `networkproto1.c`
// (NOT in `_radionet`).  Lyra mirrors as TU-scope statics in
// this translation unit — direct equivalent of reference's
// file-scope globals.  Sister-pattern to §6-B's TU-scope
// `g_metis_out_seq_num` in `wire/MetisFrame.cpp` (mirroring
// reference's TX-side `MetisOutBoundSeqNum` global at
// `networkproto1.c:30`).

// Reference: `int MetisLastRecvSeq = 0;` at
// `networkproto1.c:28`.  Last received EP6 seq number.
unsigned int g_metis_last_recv_seq = 0;

// Lyra-native sister-flag — reference doesn't need it because
// MetisLastRecvSeq starts at 0 and a 0-seq incoming datagram
// either matches (no error) or doesn't (error reported).  Lyra's
// equivalent: treat the first observed seq as the baseline
// (no error reported) and start sequence-checking from the
// second.  Without this flag, the first datagram would always
// register a spurious seq error.  This is the C↔C++23
// strict-initialization translation of reference's "first
// frame is unchecked because there's no prior to compare to."
bool g_seq_seen = false;

// Reference: `int SeqError = 0;` at `networkproto1.c:26`.
// Count of EP6 seq errors.
int g_seq_error = 0;

// Reference: `unsigned char ControlBytesIn[5];` at
// `network.h:414`.  Cached C&C-in header bytes from the most
// recent USB frame.
unsigned char g_control_bytes_in[5] = {0, 0, 0, 0, 0};

// Reference: `unsigned char* FPGAReadBufp;` at
// `network.h:498` (NOT in `_radionet`).  HL2 P1 EP6 raw
// receive buffer (1024 bytes).  Sized once at start().
// Sister of `g_fpga_write_bufp` (Ep2SendThread Stage 4C).
//
// Stage 4B.1 (sign-off 2026-06-06): corrects a Stage 4B
// mismapping that put this buffer in `prn->ReadBufp`.  Per
// reference: `prn->ReadBufp` is the P2 inbound buffer
// (network.c:667), HL2 P1 uses the separate file-scope
// `FPGAReadBufp` global.  Lyra now mirrors verbatim.
std::vector<std::uint8_t> g_fpga_read_bufp;

}  // namespace

// ---- ctor/dtor ----

Ep6RecvThread::Ep6RecvThread() {
    // §1-C Stage 4B: buffer pre-sizing moved into start() since
    // the buffers now live in `prn->...` which only becomes
    // valid after the wire-layer initializer sets the `prn`
    // global pointer at session-open.
}

Ep6RecvThread::~Ep6RecvThread() {
    stop();
}

// ---- lifecycle ----

void Ep6RecvThread::start(int socket_fd) {
    if (running_.load(std::memory_order_acquire)) return;

    // §1-C Stage 4B: `prn` must be a valid RadioNet by session-
    // open contract (reference's `prn` global is always valid
    // when MetisReadThreadMainLoop_HL2 runs — Lyra mirrors).
    if (prn == nullptr) return;

    socket_fd_     = socket_fd;
    stop_request_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);

    // Size the §1.1-reverted buffers in RadioNet.  Reference
    // does this via `calloc` at `networkproto1.c:427-428`
    // (FPGAReadBufp + FPGAWriteBufp) and inside StartMetis for
    // `RxBuff[]` / `TxReadBufp`.  Lyra centralizes here so the
    // §1.1-revert state lifecycle is one-shot at thread start.
    prn->RxBuff.assign(kMaxDdc, std::vector<double>(2 * kMaxSprPerFrame, 0.0));
    prn->TxReadBufp.assign(4 * kMaxSprPerFrame, 0.0);

    // §1-C Stage 4B.1: HL2 P1 EP6 raw receive buffer is the
    // reference's file-scope `FPGAReadBufp` (network.h:498) —
    // NOT `prn->ReadBufp` (which is P2-only).  Sized once
    // here mirroring reference's `FPGAReadBufp = (unsigned
    // char*)calloc(1024, sizeof(unsigned char));` at
    // `networkproto1.c:427`.
    g_fpga_read_bufp.assign(kEp6DatagramBytes, 0);

    // Reset TU-scope seq tracking — sister-pattern of reference
    // re-zeroing `SeqError = 0;` at `networkproto1.c:425`.
    g_metis_last_recv_seq = 0;
    g_seq_seen            = false;
    g_seq_error           = 0;
    std::memset(g_control_bytes_in, 0, sizeof(g_control_bytes_in));

    // Reference re-zeroes `mic_decimation_count` at thread entry
    // (`networkproto1.c:424`).  Mirror that here so a session
    // restart starts with a clean decimator phase regardless of
    // where the previous session ended.
    mic_decimation_count = 0;

#if defined(_WIN32)
    // §1-C Stage 4B / §5.10 fix:  set up WSAEventSelect for the
    // EP6 socket FD_READ notification — direct mirror of
    // reference `networkproto1.c:433-434`:
    //     prn->hDataEvent = WSACreateEvent();
    //     WSAEventSelect(listenSock, prn->hDataEvent, FD_READ);
    prn->hDataEvent = WSACreateEvent();
    if (prn->hDataEvent != WSA_INVALID_EVENT) {
        WSAEventSelect(static_cast<SOCKET>(socket_fd_),
                       prn->hDataEvent,
                       FD_READ);
    }
#endif

    thread_ = std::make_unique<std::thread>([this] { this->run_loop(); });
}

void Ep6RecvThread::stop() {
    stop_request_.store(true, std::memory_order_release);

#if defined(_WIN32)
    // §1-C Stage 4B / §5.10 fix:  manually signal the WSA event
    // so the WSAWaitForMultipleEvents in run_loop wakes
    // immediately + observes stop_request_.  Reference shuts
    // down via `io_keep_running = 0` + handle close; Lyra
    // mirrors by setting the event manually (semantically
    // identical — a "wake the wait" mechanism that doesn't
    // wait for socket activity).
    if (prn != nullptr && prn->hDataEvent != WSA_INVALID_EVENT) {
        WSASetEvent(prn->hDataEvent);
    }
#endif

    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    running_.store(false, std::memory_order_release);

#if defined(_WIN32)
    // Free the WSA event handle now that the thread has joined.
    if (prn != nullptr && prn->hDataEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(prn->hDataEvent);
        prn->hDataEvent = WSA_INVALID_EVENT;
    }
#endif
}

// ---- sink registration ----

void Ep6RecvThread::set_router(Router* router, int router_id) {
    router_    = router;
    router_id_ = router_id;
}

void Ep6RecvThread::set_telemetry_sink(Ep6TelemetrySink sink) {
    telemetry_sink_ = std::move(sink);
}

void Ep6RecvThread::set_mic_sink(Ep6MicSink sink) {
    mic_sink_ = std::move(sink);
}

void Ep6RecvThread::set_i2c_sink(Ep6I2cSink sink) {
    i2c_sink_ = std::move(sink);
}

// ---- reader thread ----

void Ep6RecvThread::run_loop() {
#if defined(_WIN32)
    // MMCSS Pro Audio class — best-effort.  Ignore failure.
    DWORD mmcss_task = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio",
                                                        &mmcss_task);
#endif

#if defined(_WIN32)
    // §1-C Stage 4B / §5.10 fix:  WSAWaitForMultipleEvents +
    // WSAEnumNetworkEvents + recv pattern — direct mirror of
    // reference `networkproto1.c:439-463`:
    //
    //   while (io_keep_running != 0) {
    //     DWORD retVal = WSAWaitForMultipleEvents(1, &prn->hDataEvent,
    //                       FALSE, prn->wdt ? 3000 : WSA_INFINITE,
    //                       FALSE);
    //     if (retVal == WSA_WAIT_FAILED || retVal == WSA_WAIT_TIMEOUT) {
    //       HaveSync = 0;  // send console LOS
    //       continue;
    //     } else {
    //       WSAEnumNetworkEvents(listenSock, prn->hDataEvent,
    //                            &prn->wsaProcessEvents);
    //       if (prn->wsaProcessEvents.lNetworkEvents & FD_READ) {
    //         if (prn->wsaProcessEvents.iErrorCode[FD_READ_BIT] != 0)
    //           break;
    //         MetisReadDirect(FPGAReadBufp);
    //         <process the datagram>
    //       }
    //     }
    //   }
    while (!stop_request_.load(std::memory_order_acquire)) {
        if (prn == nullptr || prn->hDataEvent == WSA_INVALID_EVENT) break;

        const DWORD timeout = (prn->wdt) ? 3000UL : WSA_INFINITE;
        DWORD retVal = WSAWaitForMultipleEvents(1, &prn->hDataEvent,
                                                FALSE, timeout, FALSE);

        // Check stop after wait wakes (stop() manually signals
        // the event to break the wait).
        if (stop_request_.load(std::memory_order_acquire)) break;

        if (retVal == WSA_WAIT_FAILED || retVal == WSA_WAIT_TIMEOUT) {
            // Reference does HaveSync = 0 + destroy_pro(prop)
            // here; Lyra-side LOS handling deferred to the
            // operator-policy commit (Task #114).  Continue.
            continue;
        }

        WSAEnumNetworkEvents(static_cast<SOCKET>(socket_fd_),
                             prn->hDataEvent,
                             &prn->wsaProcessEvents);
        if (!(prn->wsaProcessEvents.lNetworkEvents & FD_READ)) continue;
        if (prn->wsaProcessEvents.iErrorCode[FD_READ_BIT] != 0) break;

        socket_recv_len_t n = ::recv(
            socket_fd_,
            reinterpret_cast<char*>(g_fpga_read_bufp.data()),
            static_cast<socket_recv_size_t>(g_fpga_read_bufp.size()),
            0);
        if (n <= 0) continue;
        if (static_cast<std::size_t>(n) < kEp6DatagramBytes) continue;
        process_datagram(g_fpga_read_bufp.data(),
                         static_cast<std::size_t>(n));
    }
#else
    // Non-Win32: fallback to blocking recv loop.  Reference is
    // Win32-only; Lyra's Linux/macOS support is a future-ANAN
    // concern, deferred.
    while (!stop_request_.load(std::memory_order_acquire)) {
        if (prn == nullptr) break;
        socket_recv_len_t n = ::recv(
            socket_fd_,
            reinterpret_cast<char*>(g_fpga_read_bufp.data()),
            static_cast<socket_recv_size_t>(g_fpga_read_bufp.size()),
            0);
        if (n <= 0) continue;
        if (static_cast<std::size_t>(n) < kEp6DatagramBytes) continue;
        process_datagram(g_fpga_read_bufp.data(),
                         static_cast<std::size_t>(n));
    }
#endif

#if defined(_WIN32)
    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
#endif
}

// ---- per-datagram processing ----

void Ep6RecvThread::process_datagram(const uint8_t* data,
                                     std::size_t   size) {
    if (size < kEp6DatagramBytes) return;

    // 4-byte BE sequence at offset 4 of the HPSDR header.
    const uint32_t seq =
        (static_cast<uint32_t>(data[4]) << 24) |
        (static_cast<uint32_t>(data[5]) << 16) |
        (static_cast<uint32_t>(data[6]) <<  8) |
        (static_cast<uint32_t>(data[7]));

    // §1-C Stage 4B: seq tracking via TU-scope statics
    // mirroring reference's file-scope `MetisLastRecvSeq` /
    // `SeqError` at `networkproto1.c:26-28` (these are NOT in
    // `_radionet`).  §1-C Stage 1 precedent: diagnostic counters
    // with no reference counterpart are dropped — `rx_datagrams_`
    // accordingly removed.
    if (g_seq_seen) {
        const uint32_t expected = g_metis_last_recv_seq + 1;
        if (seq != expected) {
            ++g_seq_error;
        }
    }
    g_metis_last_recv_seq = seq;
    g_seq_seen            = true;

    // Two USB frames per datagram.  Each frame is dispatched
    // independently — reference loops `for (frame=0; frame<2;
    // frame++)` and runs the full unpack→switch→mic harvest
    // sequence per frame.
    process_usb_frame(data + kHpsdrHeaderBytes);
    process_usb_frame(data + kHpsdrHeaderBytes + kUsbFrameBytes);
}

// ---- USB frame parsing (mirrors `networkproto1.c:470-580`) ----

void Ep6RecvThread::process_usb_frame(const uint8_t* frame) {
    // Sync check: all three sync bytes must be 0x7F.  Reference
    // `if ((bptr[0]==0x7f) && (bptr[1]==0x7f) && (bptr[2]==0x7f))`.
    if (frame[0] != 0x7F || frame[1] != 0x7F || frame[2] != 0x7F) {
        return;
    }

    // §1-C Stage 4B: prn must be valid for all buffer access.
    if (prn == nullptr) return;

    // Cache the 5-byte C&C-in (status header) and decode.
    // §1-C Stage 4B: `g_control_bytes_in` TU-scope mirror of
    // reference's file-scope `ControlBytesIn[5]` at
    // `network.h:414` (NOT in `_radionet`).
    std::memcpy(g_control_bytes_in, frame + 3, 5);
    decode_status_header(g_control_bytes_in);

    // Dynamic per-frame layout (matches the reference exactly):
    //   stride = 6 * nddc + 2   bytes per sample slot
    //   spr    = 504 / stride   samples per DDC per frame
    const int n = nddc;
    if (n < 1) return;
    const int stride = kBytesPerDdc * n + kMicTrailerBytes;
    int spr = (stride > 0) ? (kSampleAreaBytes / stride) : 0;
    if (spr <= 0) return;
    if (spr > kMaxSprPerFrame) spr = kMaxSprPerFrame;
    const int clamped_ddc = (n <= kMaxDdc) ? n : kMaxDdc;

    // §1-C Stage 4B: `prn->RxBuff` is the §1.1-reverted host
    // for the per-DDC IQ staging.  Defensive bounds check so
    // we don't blow past a wrongly-sized RxBuff.
    if (static_cast<int>(prn->RxBuff.size()) < clamped_ddc) return;
    if (static_cast<int>(prn->TxReadBufp.size()) < 4 * spr) return;

    // ---- Per-DDC IQ unpack (DDC-major, matches reference structure)
    //
    // Reference `networkproto1.c:528-542`:
    //   for (iddc = 0; iddc < nddc; iddc++)
    //     for (isample = 0; isample < spr; isample++)
    //       k = 8 + isample*(6*nddc+2) + iddc*6
    //       prn->RxBuff[iddc][2*isample+0] = const * (bptr[k+0]<<24 | bptr[k+1]<<16 | bptr[k+2]<<8)
    //       prn->RxBuff[iddc][2*isample+1] = const * (bptr[k+3]<<24 | bptr[k+4]<<16 | bptr[k+5]<<8)
    for (int iddc = 0; iddc < clamped_ddc; ++iddc) {
        double* const dst = prn->RxBuff[iddc].data();
        for (int isample = 0; isample < spr; ++isample) {
            const int k = kFrameSampleAreaOffset
                        + isample * stride
                        + iddc * kBytesPerDdc;
            dst[2 * isample + 0] = unpack_iq_be(frame + k);
            dst[2 * isample + 1] = unpack_iq_be(frame + k + 3);
        }
    }

    // ---- Per-nddc switch (`networkproto1.c:544-558`) ----
    //
    // Direct xrouter calls for unpaired DDCs; twist() (which
    // calls xrouter internally with 2*spr) for paired DDCs.
    switch (n) {
    case 2:
        if (router_) {
            twist(spr,
                  prn->RxBuff[0].data(), prn->RxBuff[1].data(),
                  prn->TxReadBufp.data(),
                  /*source=*/0,
                  router_, router_id_);
        }
        break;
    case 4:
        if (router_) {
            // source 0 = DDC0; source 1 = twist(DDC2, DDC3);
            // source 2 = DDC1.  Verbatim from
            // `networkproto1.c:549-552`.
            xrouter(router_, router_id_, 0, spr, prn->RxBuff[0].data());
            twist(spr,
                  prn->RxBuff[2].data(), prn->RxBuff[3].data(),
                  prn->TxReadBufp.data(),
                  /*source=*/1,
                  router_, router_id_);
            xrouter(router_, router_id_, 2, spr, prn->RxBuff[1].data());
        }
        break;
    case 5:
        // ANAN P1 — kept structurally for parity; bench-untested
        // in this Lyra build (no ANAN P1 hardware available).
        // Reference `networkproto1.c:554-557`.
        if (router_ && clamped_ddc >= 5) {
            twist(spr,
                  prn->RxBuff[0].data(), prn->RxBuff[1].data(),
                  prn->TxReadBufp.data(),
                  /*source=*/0,
                  router_, router_id_);
            twist(spr,
                  prn->RxBuff[3].data(), prn->RxBuff[4].data(),
                  prn->TxReadBufp.data(),
                  /*source=*/1,
                  router_, router_id_);
            xrouter(router_, router_id_, 2, spr, prn->RxBuff[2].data());
        }
        break;
    default:
        // Other nddc values are not dispatched (reference's
        // switch has no default body).  IQ has been unpacked
        // into prn->RxBuff regardless.
        break;
    }

    // ---- Mic harvest with decimation
    //      (`networkproto1.c:560-579`) ----
    //
    // `mic_decimation_count` is post-incremented and compared
    // against `mic_decimation_factor`; on equality the counter
    // resets and the mic sample is harvested.  Default factor=0
    // (reference BSS-init) yields no harvest; HL2 default
    // operating point sets factor=1 at session open for
    // every-slot harvest.
    //
    // Mic-sample byte offset within the slot is at `k + nddc*6`
    // (mic trailer immediately follows the last DDC's IQ).  IQ
    // pair output is interleaved {I=mic, Q=0.0}, matching the
    // reference's TxReadBufp layout.
    int mic_sample_count = 0;
    for (int isamp = 0; isamp < spr; ++isamp) {
        const int k = kFrameSampleAreaOffset
                    + n * kBytesPerDdc
                    + isamp * stride;
        ++mic_decimation_count;
        if (mic_decimation_count == mic_decimation_factor) {
            mic_decimation_count = 0;
            prn->TxReadBufp[2 * mic_sample_count + 0] =
                unpack_mic_be(frame + k);
            prn->TxReadBufp[2 * mic_sample_count + 1] = 0.0;
            ++mic_sample_count;
        }
    }
    if (mic_sink_ && mic_sample_count > 0) {
        // Reference: `Inbound(inid(1, 0), mic_sample_count,
        // prn->TxReadBufp);`
        mic_sink_(mic_sample_count, prn->TxReadBufp.data());
    }
}

// ---- C&C-in header decode (`networkproto1.c:478-525`) ----
//
// I2C-overlay gate (C0 bit 7 set) takes precedence and replaces
// the telemetry-class switch entirely; otherwise the PTT/dash/
// dot shadow bits land on RadioNet and the 5-case telemetry
// switch populates per-class RADIONET fields verbatim.

void Ep6RecvThread::decode_status_header(const uint8_t cc[5]) {
    // I2C readback overlay — `networkproto1.c:478-493`.
    if (cc[0] & 0x80) {
        if (i2c_sink_) {
            i2c_sink_(cc + 1, 4);
        }
        return;
    }

    if (auto* p = prn) {
        // ptt_in / dash_in / dot_in shadows — reference at
        // `networkproto1.c:496-498`.
        //
        // Note Rule 24 preservation of reference defects:
        // `dash_in = (cc[0] << 1) & 0x01` and
        // `dot_in  = (cc[0] << 2) & 0x01` always evaluate to 0
        // (left-shifting moves bits AWAY from the LSB; the LSB
        // mask then captures the new LSB which is 0).  This is
        // a known reference defect — preserved verbatim so a
        // downstream consumer that depends on "dash/dot always
        // 0" (e.g. as a placeholder for the per-HL2 hardware
        // CW input path that is wired elsewhere) sees the same
        // wire behaviour as the reference.
        p->ptt_in  = static_cast<int>(cc[0] & 0x01);
        p->dash_in = static_cast<int>((cc[0] << 1) & 0x01);
        p->dot_in  = static_cast<int>((cc[0] << 2) & 0x01);

        // ---- 5-class telemetry switch on (cc[0] & 0xf8) ----
        //      Verbatim from `networkproto1.c:499-524`.
        switch (cc[0] & 0xf8) {
        case 0x00:  // C0 0000 0xxx
            // adc_overload + user_dig_in.
            p->adc[0].adc_overload = static_cast<int>(cc[1] & 0x01);
            p->user_dig_in = static_cast<int>((cc[1] >> 1) & 0x0f);
            break;
        case 0x08:  // C0 0000 1xxx
            // AIN5 exciter (drive) power + AIN1 PA coupler fwd.
            p->tx[0].exciter_power =
                static_cast<int>(((static_cast<int>(cc[1]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[2])       & 0x00ff));
            p->tx[0].fwd_power =
                static_cast<int>(((static_cast<int>(cc[3]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[4])       & 0x00ff));
            // FIXME (Task #114 TX-policy plumbing): reference
            // also calls `PeakFwdPower((float)prn->tx[0].fwd_power)`
            // here to maintain a running peak-meter state for
            // consumer-facing readouts.  Lyra-native peak
            // helper lands with the TX-policy plumbing commit;
            // raw fwd_power above is already populated for
            // direct consumers.
            break;
        case 0x10:  // C0 0001 0xxx
            // AIN2 PA reverse power + AIN3 MKII PA volts.
            p->tx[0].rev_power =
                static_cast<int>(((static_cast<int>(cc[1]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[2])       & 0x00ff));
            // FIXME (Task #114): reference calls
            // `PeakRevPower((float)prn->tx[0].rev_power)` here.
            p->user_adc0 =
                static_cast<int>(((static_cast<int>(cc[3]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[4])       & 0x00ff));
            break;
        case 0x18:  // C0 0001 1xxx
            // AIN4 MKII PA amps + AIN6 Hermes (supply) volts.
            p->user_adc1 =
                static_cast<int>(((static_cast<int>(cc[1]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[2])       & 0x00ff));
            p->supply_volts =
                static_cast<int>(((static_cast<int>(cc[3]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[4])       & 0x00ff));
            break;
        case 0x20:  // C0 0010 0xxx
            // Per-ADC overload bits.  Note the reference's
            // shift-then-write idiom for adc[1]/adc[2] is
            // preserved verbatim (the resulting values are 0 or
            // 2 for adc[1], 0 or 4 for adc[2] — non-1 truthy
            // values that consumers test as bool).
            p->adc[0].adc_overload = static_cast<int>(cc[1] & 0x01);
            p->adc[1].adc_overload = static_cast<int>((cc[2] & 0x01) << 1);
            p->adc[2].adc_overload = static_cast<int>((cc[3] & 0x01) << 2);
            break;
        default:
            // Reference switch has no default body — other
            // class IDs are reserved/unused.
            break;
        }
    }

    // Lyra-native telemetry-sink forwarding (additive — not
    // present in the reference).  Operator-side consumers that
    // want raw payload bytes (independent of the RADIONET
    // shadow writes above) can register a sink to receive them.
    // The shadow writes above are the reference-faithful path;
    // this sink is a convenience layer signed off as an
    // acceptable Lyra-native addition (no impact on wire or
    // RADIONET state).
    if (telemetry_sink_) {
        Ep6Telemetry tm{};
        tm.class_id   = static_cast<uint8_t>(cc[0] & 0xf8);
        tm.control[0] = cc[1];
        tm.control[1] = cc[2];
        tm.control[2] = cc[3];
        tm.control[3] = cc[4];
        telemetry_sink_(tm);
    }
}

}  // namespace lyra::wire
