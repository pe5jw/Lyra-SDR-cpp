// Lyra — C&C frame composer (§4a populated; §4b / §4c to follow).
//
// Mirrors the reference's `WriteMainLoop_HL2` round-robin
// (`networkproto1.c:869-1191`) for HL2 / HL2+ dispatch.  Per the
// signed §4a parity checkpoint (`docs/architecture/
// PARITY_CHECKPOINTS.md §4a`, 2026-06-05):
//
//   - Q1 — switch-style scheduler verbatim from the reference.
//     No data-driven cycle list; no payload map; no helpers.
//   - Q2 — eager case presence: all 19 cases (0-18) compile-time
//     in the switch.  Out-of-§4a-scope cases (1, 4-18) are
//     `assert(false …)` placeholders until §4b / §4c populate
//     them.
//   - Q3 — per-family branches inline at each case site; HL2
//     path implemented, non-HL2 paths `assert(false …)`.
//   - Q4 — single `std::mutex cc_lock_` held during the entire
//     `write_main_loop_hl2()` walk + during every state-mutating
//     setter (e.g. `set_rx_freq`).  Lock-order acyclic per §15.26
//     W1.3 / W1.4 lessons.
//   - Q5 — cursor advance once per USB frame emit (the
//     `out_control_idx_++` inside the `else` branch — no advance
//     on I2C-overlay frames).
//
// §4a in-scope cases: 0 (frame `0x00` general settings), 2 (frame
// `0x04` RX1 / DDC0), 3 (frame `0x06` RX2 / DDC1).  Case 1 (frame
// `0x02` TX VFO) lands in §4b.  Cases 4-18 (TX att, drive + PA,
// mic + LNA, RX-LNA-during-TX, ANAN-only RX freqs, PS, HL2 TX-
// latency, reset-on-disconnect) land in §4b / §4c.
//
// WIRE-INERT: built but not wired into any caller until §7
// `Ep2SendThread` ships.  The §4a code CANNOT be invoked yet
// because cases 1 / 4-18 are `assert(false)` placeholders; the
// round-robin would assert-fail on any frame whose cursor lands
// outside cases 0 / 2 / 3.

#pragma once

#include <cstdint>
#include <mutex>

namespace lyra::wire {

class FrameComposer {
public:
    FrameComposer();
    ~FrameComposer();

    // Compose one EP2 UDP datagram's worth of sync bytes + C&C
    // bytes.  Caller-owned buffer at least 1024 bytes (two USB
    // frames @ 512 bytes each).  Writes sync bytes at offsets
    // [0..2] and C0..C4 at offsets [3..7] of each USB frame.
    // Body LRIQ samples are NOT written here — that's
    // `Ep2SendThread`'s concern.
    //
    // Source mirror: `networkproto1.c::WriteMainLoop_HL2` lines
    // 869-1191.
    void write_main_loop_hl2(char* txbptr_base);

    // ===== Setter surface (§4a-scope) =====
    //
    // The reference has no setter abstraction — operator code
    // writes directly to `prn->rx[N].frequency` etc.  Lyra adds
    // these setters so the writes go through the `cc_lock_` mutex
    // (Q4 discipline), preserving thread safety the reference's
    // single-threaded I/O posture provided implicitly.

    // Write the per-RX NCO frequency that case 2 (RX1) / case 3
    // (RX2) / case 5 (RX3 mirror — §4c scope) emits.  `rx_idx` is
    // 0-based (0 = RX1, 1 = RX2, etc.); no-op if out of range or
    // `prn` is nullptr.  Source mirror: operator writes into
    // `prn->rx[N].frequency` at use sites in the reference.
    void set_rx_freq(int rx_idx, int freq_hz);

private:
    // Scheduler-internal state — NOT cross-cutting globals.
    // Source mirror: `networkproto1.c:27` (`out_control_idx`),
    // `:29` (`PreviousTXBit`).  Reference has these as file-scope
    // globals; Lyra encapsulates per-FrameComposer.
    int out_control_idx_ = 0;
    int previous_tx_bit_ = 0;

    // Single mutex covering composition + emission (Q4).
    std::mutex cc_lock_;

    // Per-case compose helpers.  Each populates C0..C4 from the
    // relevant `prn->*` / `prbpfilter->*` / §3 global state for
    // its case.  Called from inside the switch under `cc_lock_`.
    // Sources are the reference case bodies cited per-method.
    void compose_case_0(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_2(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_3(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
};

}  // namespace lyra::wire
