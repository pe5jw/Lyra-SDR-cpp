// Lyra — C&C frame composer (§4a/§4b/§4c populated; §1-C Stage 4F.2
// dissolved class → free functions).
//
// Mirrors the reference's `WriteMainLoop_HL2` round-robin
// (`networkproto1.c:869-1191`) for HL2 / HL2+ dispatch.  Per the
// signed §4a parity checkpoint (`docs/architecture/
// PARITY_CHECKPOINTS.md §4a`, 2026-06-05) AND the §1-C Stage 4F.2
// follow-up (2026-06-06):
//
//   - Q1 — switch-style scheduler verbatim from the reference.
//   - Q2 — eager case presence: all 19 cases (0-18) in the switch.
//   - Q3 — per-family branches inline at each case site; HL2 path
//     implemented, non-HL2 paths `assert(false …)`.
//   - Q4 — NO concurrency guard (Round-1 audit 2026-06-06
//     correction).  Reference is single-threaded I/O — the wire-
//     egress thread owns the C&C register state and walks the
//     scheduler; control-thread setters MUST funnel through that
//     same wire-egress thread via the command-queue primitive,
//     never touching `prn->...` directly.  An earlier `cc_lock_`
//     class-member mutex / `g_cc_lock` TU-scope static was a
//     Lyra-native deviation, removed per "do as reference, period"
//     (operator-locked 2026-06-06).
//   - Q5 — cursor advance once per USB frame emit.
//
// §1-C Stage 4F.2 (sign-off 2026-06-06): the `FrameComposer`
// class was Lyra-native scaffolding with no reference counterpart
// (reference's `WriteMainLoop_HL2` is a free function in
// `networkproto1.c` using file-scope `out_control_idx` + `PreviousTXBit`
// globals).  Dissolved into namespace-scope free functions
// operating on TU-scope statics in `FrameComposer.cpp`:
//   - `out_control_idx_` member → `g_out_control_idx` TU-scope
//     (mirror of reference `networkproto1.c:27`)
//   - `previous_tx_bit_` member → `g_previous_tx_bit` TU-scope
//     (mirror of reference `networkproto1.c:29 PreviousTXBit`)
//   - `cc_lock_` mutex → REMOVED (reference single-threaded I/O;
//     control-thread setters funnel through the wire-egress
//     thread, never touching `prn->...` directly — Round-1 audit
//     2026-06-06 correction; see Q4)
//
// Sister-pattern of §1-C Stage 2A (Router) + Stage 4D (OutboundRing)
// dissolutions.

#pragma once

#include <cstdint>

namespace lyra::wire {

// ---- Scheduler: write_main_loop_hl2 ----
//
// Compose one EP2 UDP datagram's worth of sync bytes + C&C bytes.
// Caller-owned buffer at least 1024 bytes (two USB frames @ 512
// bytes each).  Writes sync bytes at offsets [0..2] and C0..C4
// at offsets [3..7] of each USB frame.  Body LRIQ samples are
// NOT written here — that's `Ep2SendThread`'s concern.
//
// Source mirror: `networkproto1.c::WriteMainLoop_HL2` lines
// 869-1191.  Reference is single-threaded I/O — Lyra mirrors
// that contract: this function MUST be invoked from the wire-
// egress thread, and the same thread MUST own all state-mutating
// setter calls (funneled via the command-queue primitive — see
// Q4 above).
void write_main_loop_hl2(char* txbptr_base);

// ===== Setter surface (§4a-scope, §1-C Stage 4F.2: free functions) =====
//
// The reference has no setter abstraction — operator code writes
// directly to `prn->rx[N].frequency` etc. from its single I/O
// thread.  Lyra adds these typed setters for compile-time
// per-family branching + bounds checking, but per Q4 they MUST
// be invoked on the wire-egress thread (control threads enqueue
// to the command-queue primitive; the egress thread drains the
// queue and calls these setters between scheduler walks — same
// single-threaded contract as the reference).

// Write the per-RX NCO frequency that case 2 (RX1) / case 3 (RX2)
// / case 5 (RX3 mirror) emits.  `rx_idx` is 0-based (0 = RX1,
// 1 = RX2, etc.); no-op if out of range or `prn` is nullptr.
void set_rx_freq(int rx_idx, int freq_hz);

// §4b-1.8 — Write the TX NCO frequency that case 1 emits
// (frame `0x02`).  Writes `prn->tx[0].frequency`.  No-op if
// `prn` is nullptr.  Must be called from the wire-egress thread
// (see Q4).
void set_tx_freq(int freq_hz);

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
// For HL2 / HL2+ (`hpsdrModel == HPSDRModel::HERMESLITE`), applies
// the `(31 - signed_db) & 0x3F` inversion per the source-verified
// `console.cs:10657-10663` HL2-specific branch.  For non-HL2
// families, writes the raw value `& 0x3F`.
//
// Operator-axis input is signed dB (-28..+31 on HL2 per the
// hardware's bipolar range; 0..31 on ANAN-class).  Writes
// `prn->adc[0].tx_step_attn`; case 4 emits the 5-bit truncated
// form, case 11 emits the 6-bit + `0x40` enable form on the MOX
// branch.
//
// The Display.TXAttenuatorOffset panadapter compensation + the
// m_bATTonTX policy gate are operator-layer concerns deferred to
// Task #114.  This setter just writes the wire-encoded field.
void set_tx_step_attn_db(int signed_db);

// §4b-2.5 RX step attenuator setter — no inversion (any family).
// Per-ADC variant for ANAN multi-ADC radios; HL2 uses only
// `adc_idx = 0`.
void set_rx_step_attn_db(int signed_db, int adc_idx = 0);

}  // namespace lyra::wire
