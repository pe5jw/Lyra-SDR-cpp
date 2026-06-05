// Lyra — EP2 send thread implementation.  See Ep2SendThread.h.
//
// Mirrors `ChannelMaster/networkproto1.c::sendProtocol1Samples`
// (`:1204-1267`) + `MetisWriteFrame` (`:216-237`) verbatim per
// the signed §6 parity checkpoint.  All reference quirks
// (floor/ceil sign-split quantization, HL2 4-bit / non-HL2 3-bit
// CW state-bit overlay on TX I-LSBs, post-increment BE seq pack,
// MOX-off IQ zeroing as host-side defense-in-depth) are
// preserved per Rule 24.

#include "wire/Ep2SendThread.h"
#include "wire/FrameComposer.h"
#include "wire/OutboundRing.h"
#include "wire/RadioNet.h"

#include <algorithm>
#include <cmath>
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
  using socket_send_len_t  = int;
  using socket_send_size_t = int;
#else
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_send_len_t  = ssize_t;
  using socket_send_size_t = size_t;
#endif

namespace lyra::wire {

// ---- constants mirroring the reference layout ----
namespace {
// EP2 outbound datagram: 8-byte HPSDR header + 1024-byte payload.
// Reference: `unsigned char framebuf[1032];` at
// `networkproto1.c:218-220`.
constexpr std::size_t kHpsdrHeaderBytes = 8;
constexpr std::size_t kEp2DatagramBytes =
    kHpsdrHeaderBytes + 1024;  // 1032

// EP2 endpoint identifier.  Reference: `MetisWriteFrame(0x02,
// FPGAWriteBufp);` at `:1198`.
constexpr int kEp2Endpoint = 0x02;

// Sample-slot counts mirroring the per-USB-frame budget.
constexpr int kSampleSlotsPerFrame = 63;       // = 504/8 bytes
constexpr int kSampleSlotsPerDatagram = 2 * kSampleSlotsPerFrame;  // 126
constexpr int kLriqBytesPerFrame = 8 * kSampleSlotsPerFrame;       // 504

// ---- metis_write_frame ----
//
// Free function mirroring `int MetisWriteFrame(int endpoint, char*
// bufp)` at `networkproto1.c:216-237`.  Composes the 8-byte HPSDR
// header (`0xEF 0xFE 0x01 endpoint` + 4-byte BE seqnum), memcpys
// 1024 bytes of payload, and calls `sendto` on the bound socket.
// Returns the payload bytes sent (positive) or a negative socket
// error code.
//
// Per the locked §6 Q3 sign-off, this lives in this translation
// unit's anonymous namespace (not a class member) and uses the
// caller-supplied `seq_num_ref` reference for the post-increment
// — `out_seq_num_` is owned by `Ep2SendThread` and threaded
// through here.
int metis_write_frame(int                socket_fd,
                      const void*        dest_addr,
                      std::size_t        dest_addrlen,
                      int                endpoint,
                      const uint8_t*     payload,
                      std::atomic<uint32_t>& seq_num_ref) {
    std::array<uint8_t, kEp2DatagramBytes> framebuf{};

    // 4-byte HPSDR sync prefix (`:223-226`).
    framebuf[0] = 0xEF;
    framebuf[1] = 0xFE;
    framebuf[2] = 0x01;
    framebuf[3] = static_cast<uint8_t>(endpoint);

    // 4-byte outbound sequence number, BE pack (`:221, 227-231`).
    // Reference uses a cast-to-byte-pointer manual byte-swap that
    // relies on little-endian host; Lyra uses explicit shifts that
    // are endian-portable and produce the IDENTICAL 4 wire bytes.
    const uint32_t seq = seq_num_ref.load(std::memory_order_acquire);
    framebuf[4] = static_cast<uint8_t>((seq >> 24) & 0xff);
    framebuf[5] = static_cast<uint8_t>((seq >> 16) & 0xff);
    framebuf[6] = static_cast<uint8_t>((seq >>  8) & 0xff);
    framebuf[7] = static_cast<uint8_t>( seq        & 0xff);
    // Post-increment matches reference `++MetisOutBoundSeqNum;`
    // at `:231`.
    seq_num_ref.fetch_add(1, std::memory_order_release);

    // Payload copy (`:232`).
    std::memcpy(framebuf.data() + kHpsdrHeaderBytes, payload, 1024);

    // sendto (`:234`).  Reference uses `sendPacket(...)` which
    // wraps `sendto` with the configured destination port.
    const socket_send_len_t result = ::sendto(
        socket_fd,
        reinterpret_cast<const char*>(framebuf.data()),
        static_cast<socket_send_size_t>(kEp2DatagramBytes),
        0,
        static_cast<const sockaddr*>(dest_addr),
        static_cast<int>(dest_addrlen));

    if (result < 0) {
        return -1;
    }
    // Return payload bytes (matches reference `result -= 8;`).
    return static_cast<int>(result) - static_cast<int>(kHpsdrHeaderBytes);
}
}  // namespace

// ---- ctor/dtor ----

Ep2SendThread::Ep2SendThread()
    : out_buf_(kLriqBytesPerDatagram, 0),
      fpga_write_buf_(kFpgaPayloadBytes, 0) {}

Ep2SendThread::~Ep2SendThread() {
    stop();
}

// ---- lifecycle ----

void Ep2SendThread::start(int                socket_fd,
                          const void*        dest_addr,
                          std::size_t        dest_addrlen,
                          FrameComposer*     composer,
                          OutboundRing*      ring) {
    if (running_.load(std::memory_order_acquire)) return;
    socket_fd_     = socket_fd;
    dest_addr_     = dest_addr;
    dest_addrlen_  = dest_addrlen;
    composer_      = composer;
    ring_          = ring;
    stop_request_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    thread_ = std::make_unique<std::thread>([this] { this->run_loop(); });
}

void Ep2SendThread::stop() {
    stop_request_.store(true, std::memory_order_release);
    // Wake the consumer parked in `wait_pair_ready()` so it can
    // observe stop_request_ + exit cleanly.
    if (ring_) ring_->unblock();
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    running_.store(false, std::memory_order_release);
}

// ---- reader thread ----

void Ep2SendThread::run_loop() {
#if defined(_WIN32)
    // §6.1 — MMCSS Pro-Audio priority 2 per
    // `networkproto1.c:1207-1208`.  Best-effort; ignore failure.
    DWORD mmcss_task = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio",
                                                        &mmcss_task);
    if (mmcss_handle) {
        AvSetMmThreadPriority(mmcss_handle, AVRT_PRIORITY_HIGH);
    }
#endif

    while (!stop_request_.load(std::memory_order_acquire)) {
        if (!process_one_pair()) break;
    }

#if defined(_WIN32)
    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
#endif
}

// ---- per-iteration body (mirrors :1218-1265) ----

bool Ep2SendThread::process_one_pair() {
    if (!ring_ || !composer_) return false;

    // §6.2 — wait for BOTH lr_ready_ + iq_ready_ (`:1220`).
    if (!ring_->wait_pair_ready()) {
        // unblock() was called — clean shutdown.
        return false;
    }

    double* lr = ring_->lr_buf_mut();
    double* iq = ring_->iq_buf_mut();

    // §6.4 — EER mode LR-overwrite-by-IQ (`:1222-1226`).
    //
    // FIXME (Task #114): the reference's gate
    // `pcm->xmtr[0].peer->run && XmitBit` is the channel-master
    // EER/ETR-enabled flag AND the wire-MOX bit.  Lyra has no
    // channel-master equivalent of `pcm->xmtr[0].peer->run`; EER
    // mode is a v0.3+ TX feature requiring upstream WDSP TX
    // channel + envelope-tracking implementation.  Branch shape
    // preserved with a stubbed `if (false &&` guard — when EER
    // lands, the guard flips to the real condition.
    if (false && XmitBit) {
        // Reference: `memcpy(prn->outLRbufp, prn->outIQbufp + 256,
        // sizeof(complex) * 126);` — overwrite LR audio with IQ
        // data offset by 256 doubles into iq buffer.  Shape kept
        // for the eventual EER-mode landing under Task #114.
        std::memcpy(lr, iq + 256, sizeof(double) * 2 * 126);
    }

    // §6.3 — MOX-edge IQ zeroing (`:1227`).  ⚠ Host-side defense-
    // in-depth on RF emission: when not transmitting (XmitBit
    // clear), zero the IQ buffer in the wire path BEFORE
    // quantization → no non-zero TX samples reach the DAC even if
    // a producer thread races ahead of a PTT-release.  Preserved
    // verbatim per Rule 24.
    if (!XmitBit) {
        std::fill_n(iq, OutboundRing::kDoublesPerBuffer, 0.0);
    }

    // §6.5 — optional L/R channel swap (`:1231-1239`).  When
    // `prn->swap_audio_channels` is set, swap L ↔ R in-place to
    // compensate for hardware-firmware variants.  Verbatim loop
    // bounds (4 × 63 = 252 doubles = 126 LR pairs).
    if (auto* p = prn; p && p->swap_audio_channels) {
        for (int i = 0; i < 4 * kSampleSlotsPerFrame; i += 2) {
            const double swap = lr[i + 0];
            lr[i + 0] = lr[i + 1];
            lr[i + 1] = swap;
        }
    }

    // §6.6 + §6.7 — float → int16 quantization + CW state-bit
    // overlay.  Fills `out_buf_` with the 8-bytes-per-sample-slot
    // packed format (L_hi L_lo R_hi R_lo I_hi I_lo Q_hi Q_lo).
    quantize_and_pack(lr, iq);

    // §6.8 — compose C&C bytes into the FPGA write buffer
    // (`:1261-1264`, dispatched on hpsdrModel — HL2 path).
    // FrameComposer fills offsets [0..7] + [512..519] (sync + C&C)
    // of the 1024-byte buffer.  Body LRIQ at [8..511] + [520..1023]
    // is filled by the memcpy below.
    composer_->write_main_loop_hl2(
        reinterpret_cast<char*>(fpga_write_buf_.data()));

    // LRIQ memcpy (`:1193-1195`) — both USB frames.
    std::memcpy(fpga_write_buf_.data() + 8,
                out_buf_.data(),
                static_cast<std::size_t>(kLriqBytesPerFrame));
    std::memcpy(fpga_write_buf_.data() + 520,
                out_buf_.data() + kLriqBytesPerFrame,
                static_cast<std::size_t>(kLriqBytesPerFrame));

    // §6.9 — submit via metis_write_frame (`:1198`).  Held under
    // send_lock_ so concurrent sends on the same socket (e.g.,
    // priming C&C from §7 ForceCandC) cannot interleave.
    int result = 0;
    {
        std::lock_guard<std::mutex> lk(send_lock_);
        if (socket_fd_ >= 0 && dest_addr_) {
            result = metis_write_frame(socket_fd_,
                                       dest_addr_,
                                       dest_addrlen_,
                                       kEp2Endpoint,
                                       fpga_write_buf_.data(),
                                       out_seq_num_);
        }
    }
    if (result < 0) {
        send_errors_.fetch_add(1, std::memory_order_relaxed);
    } else {
        datagrams_sent_.fetch_add(1, std::memory_order_relaxed);
    }

    // §6.8 — producer-side release: signal both LR + IQ producers
    // "buffer consumed, free to refill" (`:1199-1200`).
    ring_->notify_consumed_pair();
    return true;
}

// ---- §6.6 + §6.7 quantization + CW state-bit overlay
//      (`:1241-1259`) ----

void Ep2SendThread::quantize_and_pack(const double* lr_buf,
                                      const double* iq_buf) {
    const double* const pbuffs[2] = { lr_buf, iq_buf };

    // CW state bits captured once per pair (the values are
    // updated by the CW keyer between pairs; sampling once per
    // pair is consistent with the reference's per-frame read).
    int      cw_enable = 0;
    int      cw_word_hl2 = 0;
    int      cw_word_non = 0;
    bool     is_hl2    = false;
    if (auto* p = prn) {
        cw_enable = p->cw.cw_enable;
        cw_word_hl2 = ((p->tx[0].cwx_ptt & 0x01) << 3) |
                      ((p->tx[0].dot     & 0x01) << 2) |
                      ((p->tx[0].dash    & 0x01) << 1) |
                      ( p->tx[0].cwx     & 0x01);
        cw_word_hl2 &= 0b00001111;
        cw_word_non = ((p->tx[0].dot     & 0x01) << 2) |
                      ((p->tx[0].dash    & 0x01) << 1) |
                      ( p->tx[0].cwx     & 0x01);
        cw_word_non &= 0b00000111;
        is_hl2    = (hpsdrModel == HPSDRModel::HERMESLITE);
    }

    // Reference triple-nested loop at `:1241-1259`.
    //   i = 0..125  (sample-slot index across both USB frames)
    //   j = 0..1    (j=0 = LR pair, j=1 = IQ pair)
    //   k = 0..1    (k=0 = first component, k=1 = second)
    for (int i = 0; i < kSampleSlotsPerDatagram; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                const double v = pbuffs[j][i * 2 + k];

                // Round-to-nearest with floor/ceil sign-split
                // (`:1245-1246`).  Verbatim from reference.
                int16_t temp = (v >= 0.0)
                    ? static_cast<int16_t>(std::floor(v * 32767.0 + 0.5))
                    : static_cast<int16_t>(std::ceil( v * 32767.0 - 0.5));

                // §6.7 — CW state-bit overlay on TX I-sample LSBs
                // (`:1247-1256`).  Gated on `cw_enable && j == 1`
                // (j=1 = IQ pair only).  When active, the entire
                // 16-bit sample is REPLACED with the CW state
                // word — TX I-sample LSBs are overwritten on the
                // wire.  Preserved verbatim per Rule 24 — the
                // HL2 gateware reads these bits as CW state
                // during CW transmit.
                if (cw_enable && j == 1) {
                    temp = static_cast<int16_t>(
                        is_hl2 ? cw_word_hl2 : cw_word_non);
                }

                // BE pack into out_buf_ (`:1257-1258`).
                const std::size_t off =
                    static_cast<std::size_t>(8 * i + 4 * j + 2 * k);
                out_buf_[off + 0] =
                    static_cast<uint8_t>((temp >> 8) & 0xff);
                out_buf_[off + 1] =
                    static_cast<uint8_t>( temp       & 0xff);
            }
        }
    }
}

}  // namespace lyra::wire
