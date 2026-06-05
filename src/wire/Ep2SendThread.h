// Lyra — EP2 send thread (§6 / §10.2 wire layer).
//
// std::thread, MMCSS Pro Audio priority 2 (Windows).  Mirrors the
// reference `sendProtocol1Samples` (`networkproto1.c:1204-1267`):
// pair-waits on `OutboundRing` for both LR + IQ buffers ready,
// applies per-sample TX-side transforms (MOX-edge IQ zeroing,
// optional EER mode overwrite of LR with IQ, optional L/R audio
// channel swap, float → int16 round-to-nearest quantization,
// HL2 4-bit / non-HL2 3-bit CW state-bit overlay on TX I-sample
// LSBs when CW enabled), calls `FrameComposer::write_main_loop_hl2`
// to fill the C&C bytes in the 2-USB-frame outbound buffer,
// memcpys the LRIQ payload into the same buffer at offsets [8..511]
// + [520..1023], and hands the 1024-byte composed payload to the
// `metis_write_frame(0x02, ...)` free function which prepends the
// 8-byte HPSDR header + 4-byte BE outbound sequence number and
// calls `sendto`.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lyra::wire {

class FrameComposer;  // fwd — included only in cpp
class OutboundRing;   // fwd — included only in cpp

class Ep2SendThread {
public:
    Ep2SendThread();
    ~Ep2SendThread();

    Ep2SendThread(const Ep2SendThread&)            = delete;
    Ep2SendThread& operator=(const Ep2SendThread&) = delete;

    // ---- Lifecycle ----
    //
    // `start()` requires:
    //   - `socket_fd` : the bound UDP socket (caller-owned)
    //   - `dest_addr` / `dest_addrlen` : the radio's `sockaddr`
    //     (caller-owned; lifetime must outlive this thread)
    //   - `composer`  : reference to the live FrameComposer that
    //     fills the per-USB-frame C&C header bytes
    //   - `ring`      : reference to the OutboundRing the audio
    //     mixer + TX-DSP threads push into
    void start(int                socket_fd,
               const void*        dest_addr,
               std::size_t        dest_addrlen,
               FrameComposer*     composer,
               OutboundRing*      ring);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // ---- Diagnostic counters (read from any thread) ----
    uint64_t datagrams_sent()     const { return datagrams_sent_.load(); }
    uint64_t send_errors()        const { return send_errors_.load(); }
    uint32_t out_seq_num()        const { return out_seq_num_.load(); }

private:
    void run_loop();

    // Per-iteration body — verbatim port of `sendProtocol1Samples`
    // body inside `while (io_keep_running)`.  Returns false to
    // request loop exit (e.g., on send error).
    bool process_one_pair();

    // Float → int16 round-to-nearest with floor/ceil sign-split
    // (per `networkproto1.c:1245-1246`), then BE pack into
    // `out_buf_` at the per-sample offset (per `:1257-1258`).
    // Honors the CW state-bit overlay on TX I-sample LSBs when
    // `prn->cw.cw_enable` is set (per `:1247-1256`).
    void quantize_and_pack(const double* lr_buf,
                           const double* iq_buf);

private:
    // Owning socket fd is the caller's; we just hold a copy.
    int               socket_fd_     = -1;
    const void*       dest_addr_     = nullptr;
    std::size_t       dest_addrlen_  = 0;

    // Non-owning references to the cross-thread collaborators.
    FrameComposer*    composer_      = nullptr;
    OutboundRing*     ring_          = nullptr;

    // Thread control.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_request_{false};
    std::unique_ptr<std::thread> thread_;

    // Send-side critical section — held across the
    // `metis_write_frame` `sendto` call so concurrent sends on the
    // same socket cannot interleave.  Mirrors the reference's
    // `prn->sndpktp1` (the P1 send critical section).
    std::mutex send_lock_;

    // §6 outbound buffers (§1.1 networking-infrastructure stays
    // out of RadioNet).
    //
    // `out_buf_` (= reference's `prn->OutBufp`): 1008 bytes (504
    // bytes per USB frame × 2 frames) of quantized LRIQ output,
    // filled by `quantize_and_pack` and then memcpy'd into the
    // 1024-byte FPGA buffer at offsets [8..511] + [520..1023].
    static constexpr int kLriqBytesPerFrame = 504;  // 63 sample-slots × 8 bytes/slot
    static constexpr int kLriqBytesPerDatagram = 2 * kLriqBytesPerFrame;  // 1008
    std::vector<uint8_t> out_buf_;

    // `fpga_write_buf_` (= reference's `prn->FPGAWriteBufp`): the
    // 1024-byte EP2 payload (sync+C&C+LRIQ packed for both USB
    // frames) handed to `metis_write_frame`.
    static constexpr int kFpgaPayloadBytes = 1024;
    std::vector<uint8_t> fpga_write_buf_;

    // §6.9 outbound sequence counter.  Per the locked Q3 sign-off
    // this is encapsulated as a class member rather than the
    // reference's `MetisOutBoundSeqNum` global — operator-approved
    // Lyra-native deviation per §1.1 networking-infrastructure
    // exclusion.  Atomic so diagnostic readers from other threads
    // observe a coherent value.
    std::atomic<uint32_t> out_seq_num_{0};

    // ---- Diagnostic counters ----
    std::atomic<uint64_t> datagrams_sent_{0};
    std::atomic<uint64_t> send_errors_{0};
};

}  // namespace lyra::wire
