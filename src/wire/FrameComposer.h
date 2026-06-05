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

    // §4b-1.8 — Write the TX NCO frequency that case 1 emits
    // (frame `0x02`).  Writes `prn->tx[0].frequency` under
    // `cc_lock_`.  No-op if `prn` is nullptr.
    void set_tx_freq(int freq_hz);

    // §4b-2.5 setter surface.

    // Write `prn->tx[0].drive_level` (case 10 C1).  Operator-axis
    // input (the reference's `_tx_attenuator_data` is the same
    // operator-axis value; SWR correction happens at Radio-layer
    // BEFORE calling this setter).  No-op if `prn` is nullptr.
    void set_drive_level(int level);

    // Write `prn->tx[0].pa` (case 10 C3 bit 7 — the LEGACY PA
    // enable path).  Note: on Apollo-modded HL2+, the PA enable
    // is C2 bit 3 via `ApolloTuner` global (Task #114), NOT this
    // bit.  Kept for reference parity + non-Apollo paths.
    void set_pa_on(bool on);

    // §4b-2.5 TX step attenuator setter — **per-family branching**.
    //
    // For HL2 / HL2+ (`hpsdrModel == HPSDRModel::HERMESLITE`),
    // applies the `(31 - signed_db) & 0x3F` inversion per the
    // source-verified `console.cs:10657-10663` HL2-specific branch.
    // For non-HL2 families, writes the raw value `& 0x3F`.
    //
    // Operator-axis input is signed dB (-28..+31 on HL2 per the
    // hardware's bipolar range; 0..31 on ANAN-class).  Writes
    // `prn->adc[0].tx_step_attn`; case 4 emits the 5-bit truncated
    // form, case 11 emits the 6-bit + `0x40` enable form on the
    // MOX branch.
    //
    // The Display.TXAttenuatorOffset panadapter compensation +
    // m_bATTonTX policy gate are operator-layer concerns deferred
    // to Task #114.  This setter just writes the wire-encoded
    // field.
    void set_tx_step_attn_db(int signed_db);

    // §4b-2.5 RX step attenuator setter — no inversion (any family).
    // Per-ADC variant for ANAN multi-ADC radios; HL2 uses only
    // `adc_idx = 0`.
    void set_rx_step_attn_db(int signed_db, int adc_idx = 0);

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
    void compose_case_1(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_2(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_3(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_4(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_13(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_14(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_15(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_17(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_18(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);

    // §4b-2 case helpers (TX heavyweight — §15.26 territory).
    void compose_case_10(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_11(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_12(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);
    void compose_case_16(unsigned char& C0, unsigned char& C1,
                         unsigned char& C2, unsigned char& C3,
                         unsigned char& C4);

    // §4c case helpers (RX-mirror / ANAN-only).  All 5 are
    // structurally simple BE-32 freq writes; only `C0 |=` constant
    // + ddc_freq source differ.
    void compose_case_5(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_6(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_7(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_8(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
    void compose_case_9(unsigned char& C0, unsigned char& C1,
                        unsigned char& C2, unsigned char& C3,
                        unsigned char& C4);
};

}  // namespace lyra::wire
