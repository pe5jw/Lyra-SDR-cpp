// Lyra — startup C&C priming (§7 / §10.2 wire layer).
//
// Sends N priming C&C frames at HL2Stream session-open, BEFORE
// the normal `Ep2SendThread` send loop begins.  Mirrors the
// reference `ForceCandCFrame` priming pass at
// `ChannelMaster/networkproto1.c:134-139`:
//
//   ForceCandCFrames(count, 2, prn->tx[0].frequency);   // TX freq
//   Sleep(10);
//   ForceCandCFrames(count, 4, prn->rx[0].frequency);   // RX1 freq
//   Sleep(10);
//
// Without this priming pass, post-priming RX freq updates can be
// missed by HL2 gateware per CLAUDE.md §3.2 — the duplex bit is
// deliberately NOT set in the priming frame-0 (c4 = (nddc-1)<<3
// = 0x18 for HL2 nddc=4; main-loop frame-0 sets c4 = 0x1C with
// bit 2 = duplex), so the gateware enters its "main-loop"
// dispatch state via this pre-roll.  Reference quirk preserved
// verbatim per Rule 24.
//
// Reference call site: `MetisReadThreadMainLoop_HL2` at
// `networkproto1.c:430` invokes `ForceCandCFrame(3);` at the TOP
// of the read loop, BEFORE the EP6 read loop begins consuming
// datagrams (and before any EP2 send activity).  Lyra's
// equivalent contract: `HL2Stream::session_open()` calls
// `ForceCandC::prime(3, tx_freq, rx_freq)` SYNCHRONOUSLY on the
// session-open thread, BEFORE `Ep2SendThread::start()` and
// `Ep6RecvThread::start()`.  This temporal separation is what
// allows the shared `wire/MetisFrame.cpp` TU-scope socket / seq
// state to be safely accessed by both the priming pass and the
// later send-thread with NO LOCK (matching the reference
// verbatim).
//
// Phase 2 step 14 (next commit) wires the call site into
// HL2Stream's session-open path.

#pragma once

#include <cstdint>

namespace lyra::wire {

class ForceCandC {
public:
    ForceCandC();
    ~ForceCandC();

    ForceCandC(const ForceCandC&)            = delete;
    ForceCandC& operator=(const ForceCandC&) = delete;

    // Run the priming sequence.  Mirrors reference
    // `ForceCandCFrame(int count)` at `networkproto1.c:134-139`:
    //
    //   1. Send `count` priming frames with case-2 (TX freq slot)
    //      carrying `tx_freq_hz`.
    //   2. Sleep 10 ms.
    //   3. Send `count` priming frames with case-4 (RX1 freq slot)
    //      carrying `rx_freq_hz`.
    //   4. Sleep 10 ms.
    //
    // Caller MUST have already bound the wire socket via
    // `lyra::wire::metis_wire_bind()` before invoking — priming
    // emits via the shared `lyra::wire::metis_write_frame()`
    // primitive.  Runs synchronously on the calling thread; does
    // not spawn anything.
    //
    // `count == 3` is the locked HL2 value (`networkproto1.c:430`).
    void prime(int count,
               std::int32_t tx_freq_hz,
               std::int32_t rx_freq_hz);

    // Single pass — emit `count` priming frames with `c0` (= 2 for
    // TX, 4 for RX1) and `vfo_freq` in the per-frame layout.
    // Mirrors reference `ForceCandCFrames(int count, int c0,
    // int vfofreq)` at `networkproto1.c:106-132`.  Exposed
    // separately for symmetry with the reference + for unit
    // testability of the per-pass byte layout.
    void prime_pass(int count,
                    int c0,
                    std::int32_t vfo_freq);
};

}  // namespace lyra::wire
