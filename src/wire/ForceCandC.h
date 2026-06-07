// Lyra — startup C&C priming (§7 / §10.2 wire layer).
//
// Direct mirror of the reference's `ForceCandCFrames` +
// `ForceCandCFrame` free functions at
// `ChannelMaster/networkproto1.c:106-139`.  Both are file-scope
// free functions in the reference — no class, no state, no
// shared object.  Per the operator-locked "do as reference,
// period" rule (2026-06-06), Lyra's port is also free functions
// — sibling-pattern of §1-C Stage 2A (Router), Stage 4D
// (OutboundRing), and Stage 4F.2 (FrameComposer) dissolutions
// (the §1-C audit missed ForceCandC; Round-1 audit 2026-06-06
// caught it).
//
// Reference call site: `MetisReadThreadMainLoop_HL2` at
// `networkproto1.c:430` invokes `ForceCandCFrame(3);` AT THE
// TOP OF THE READ LOOP, BEFORE the EP6 read loop begins
// consuming datagrams (and before any EP2 send activity).  The
// reference takes ONE argument (count) — it reads
// `prn->tx[0].frequency` and `prn->rx[0].frequency` directly
// from the global `prn` struct.  Lyra mirrors that signature
// verbatim per "do as reference, period" — the earlier
// `prime(count, tx_freq_hz, rx_freq_hz)` Lyra-native arg list
// was a deviation caught by the same audit (Round-1 2026-06-06).
//
// Lyra's equivalent contract: called from
// `Ep6RecvThread::run_loop` body at TOP, before the WSA wait
// loop begins.  Synchronous on whatever thread invokes; emits
// via the shared `lyra::wire::metis_write_frame()` primitive
// (and thus shares the `g_metis_out_seq_num` counter).

#pragma once

#include <cstdint>

namespace lyra::wire {

// `void ForceCandCFrames(int count, int c0, int vfofreq)`
// — `networkproto1.c:106-132`.
//
// Single pass — emit `count` priming frames with `c0` (= 2 for
// TX, 4 for RX1) and `vfo_freq` BE-packed into c1..c4 of the
// second USB frame.  Frame 0 carries the fixed C&C header
// (`SampleRateIn2Bits & 3`, `(nddc-1) << 3`).  Caller MUST have
// bound the wire socket via `metis_wire_bind()` first.  No-op
// if `prn == nullptr` (defensive — though the reference would
// just crash, Lyra has the early-return safety per the same
// pattern as `metis_write_frame`).
void force_candc_frames(int count, int c0, std::int32_t vfo_freq);

// `void ForceCandCFrame(int count)` — `networkproto1.c:134-139`.
//
//   ForceCandCFrames(count, 2, prn->tx[0].frequency);
//   Sleep(10);
//   ForceCandCFrames(count, 4, prn->rx[0].frequency);
//   Sleep(10);
//
// Two passes (TX-freq c0=2 + RX1-freq c0=4) with 10 ms
// inter-pass sleeps.  Reads TX + RX1 freqs directly from the
// `prn` global per the reference — no args beyond `count`.  HL2
// call site uses `count == 3` (`networkproto1.c:430`).  No-op
// if `prn == nullptr`.
void force_candc_frame(int count);

}  // namespace lyra::wire
