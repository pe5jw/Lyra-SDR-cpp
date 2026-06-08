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
// `lyra::wire::metis_write_frame(0x02, ...)` free function which
// prepends the 8-byte HPSDR header + 4-byte BE outbound sequence
// number (TU-scope `g_metis_out_seq_num` per §6-B) and calls
// `sendto` on the TU-scope bound socket.
//
// §6-B parity correction (sign-off 2026-06-06):  the outbound
// sequence counter (was `out_seq_num_` member) + the send-lock
// (was `send_lock_` member) + the socket/dest copies (were
// `socket_fd_` / `dest_addr_` / `dest_addrlen_` members) all
// moved to `wire/MetisFrame.{h,cpp}` TU-scope to match the
// reference's file-scope globals + free-function + no-lock
// structure verbatim.  This thread's `start()` is now a thin
// caller of `lyra::wire::metis_wire_bind()` that also stores the
// `FrameComposer` / `OutboundRing` pointers it needs.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace lyra::wire {

class Ep2SendThread {
public:
    Ep2SendThread();
    ~Ep2SendThread();

    Ep2SendThread(const Ep2SendThread&)            = delete;
    Ep2SendThread& operator=(const Ep2SendThread&) = delete;

    // ---- Lifecycle ----
    //
    // `start()` requires `socket_fd` for parity with the API the
    // §10.3 step 14 wire-up will call.  The actual socket binding
    // happens ONCE in the session-open path (hl2_stream.cpp) via
    // `metis_wire_bind()`; reference does the equivalent once at
    // StartMetis discovery + never per-thread.  Caller is
    // responsible for `outbound_init()` at session-open.
    //
    // §1-C Stage 4D: `OutboundRing* ring` param removed (free
    // functions).  §1-C Stage 4F.2: `FrameComposer* composer`
    // param removed (free functions).
    void start(int socket_fd);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // §1-C (2026-06-06): `datagrams_sent()` + `send_errors()`
    // diagnostic accessors removed.  Reference has no equivalent
    // aggregate counters in `sendProtocol1Samples` — per the
    // strict "do as reference, period" directive these Lyra-
    // native observability additions are reverted.  The
    // §6-B `out_seq_num()` accessor was already removed (counter
    // is now TU-scope `lyra::wire::metis_out_seq_num()` in
    // `wire/MetisFrame.cpp`).

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
    // §6-B (sign-off 2026-06-06): socket / dest_addr / dest_addrlen
    // are NO LONGER stored as members.  Caller passes them to
    // `start()` which forwards once to `lyra::wire::metis_wire_bind()`;
    // the TU-scope state in `wire/MetisFrame.cpp` is the single
    // source of truth — direct mirror of the reference's file-scope
    // `listenSock` global + `prn->base_outbound_port`.  §6 Q3/Q5
    // members + the `send_lock_` mutex are deleted per the same
    // reference-fidelity sweep.

    // §1-C Stage 4D + 4F.2: collaborator members removed —
    // OutboundRing + FrameComposer both dissolved into
    // namespace-scope free functions in their respective .cpp
    // files.  Ep2SendThread is now a pure thread orchestrator.

    // Thread control.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_request_{false};
    std::unique_ptr<std::thread> thread_;

    // §1-C Stage 4C (sign-off 2026-06-06): §1.1 networking-
    // infrastructure exclusion REVERTED.  The two outbound
    // buffers live at their reference homes:
    //
    // - `prn->OutBufp` (1024 bytes of quantized LRIQ output) —
    //   IS in `_radionet` per network.h:64.  Reference accesses
    //   it at networkproto1.c:1257-1258 + :1262-1264
    //   (`WriteMainLoop_HL2(prn->OutBufp);`).
    // - `g_fpga_write_bufp` (1024-byte EP2 payload) — moved to
    //   `wire/FrameComposer.cpp` TU-scope (2026-06-08, Task #121,
    //   operator directive "do as reference, period").
    //   Reference `FPGAWriteBufp` (network.h:499) is a file-
    //   scope global, NOT in `_radionet` — sister-pattern of
    //   §6-B `g_metis_out_seq_num` + §4B.1 `g_fpga_read_bufp`.
    //   It lives next to the function that uses it (FrameComposer
    //   in Lyra, networkproto1.c in reference) — both are
    //   `WriteMainLoop_HL2`'s data.
};

}  // namespace lyra::wire
