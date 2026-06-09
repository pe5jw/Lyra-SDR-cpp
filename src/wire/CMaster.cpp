// Lyra-cpp — CMaster.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/cmaster.c (specifically the
//   `cm` / `pcm` globals at :29-30, the `SendpOutbound*` setters
//   at :407-432, and `SetTCIRun` at :434-437)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014-2019 Warren Pratt, NR0V
// License: GNU General Public License v3 or later
//
// See CMaster.h for the multi-stage port plan + per-symbol
// reference-mapping rationale + full C → C++23 idiom-translation
// list.  See NOTICE.md / CREDITS.md at repo root for full
// upstream attribution.
//
// ---------------------------------------------------------------
//
// Stage A (this commit): module shell.  cmaster struct typedef +
// global pcm pointer + enum AudioCodecId + 4 SendpOutbound* setter
// stubs.  NO behaviour change — Stage A populates the architectural
// shell that subsequent stages (aamix port, ilv port, xcmaster pump
// body, etc.) plug into.

#include "wire/CMaster.h"
#include "wdsp/AAMix.h"
#include "wdsp/ILV.h"

namespace lyra::wire {

// Reference cmaster.c:29-30 — `cmaster cm = {0}; CMASTER pcm = &cm;`
// Process-lifetime; never freed.  Default-constructed via in-class
// initializers in CMasterState; equivalent to reference's `{0}`
// zero-init for the populated fields, with the default
// `audioCodecId = HERMES` matching reference's eventual session-open
// per-radio-family init writing the same value for HL2/HL2+.
CMasterState  cm  {};
CMasterState* pcm = &cm;

// ===== SendpOutboundRx =====
//
// Reference cmaster.c:407-412:
//   void SendpOutboundRx (void (*Outbound)(int id, int nsamples, double* buff)) {
//       pcm->OutboundRx = Outbound;
//       SetAAudioMixOutputPointer (0, 0, pcm->OutboundRx);
//   }
//
// Stage A: stores callback only.  The
// `SetAAudioMixOutputPointer(0, 0, pcm->OutboundRx)` call lands when
// Stage B (aamix port) ships — at that point the call wires the
// stored callback into the AAMIX output dispatcher so RX audio frames
// flow through the registered callback every time AAMIX produces.

void SendpOutboundRx(OutboundCallback cb)
{
    pcm->OutboundRx = std::move(cb);
    // Stage B.5 wire-up: push the registered callback into the
    // ported AAMix's central pointer-bank slot 0 (the conventional
    // RX mixer id matching reference cmaster.c:411
    // `SetAAudioMixOutputPointer(0, 0, pcm->OutboundRx)`).  Safe
    // when no AAMix exists at that slot yet (paamix[0] == nullptr
    // before Stage B.6's create_aamix call site): the setter's
    // resolve_aamix returns nullptr and the setter early-returns
    // without effect.  Once Stage B.6 ports the RX audio path to
    // construct AAMix at id=0, this call wires the operator-
    // registered RX-out callback into mix_main's Outbound dispatch
    // automatically; no further plumbing change needed.
    lyra::wdsp::SetAAudioMixOutputPointer(nullptr, 0, pcm->OutboundRx);
}

// ===== SendpOutboundTx =====
//
// Reference cmaster.c:414-419:
//   void SendpOutboundTx(void (*Outbound)(int id, int nsamples, double* buff)) {
//       pcm->OutboundTx = Outbound;
//       SetILVOutputPointer(0, pcm->OutboundTx);
//   }
//
// Stage C.3 wire-up: push the registered callback into ILV's
// central pointer-bank slot 0 (the conventional TX-out interleaver
// id matching reference cmaster.c:418 `SetILVOutputPointer(0,
// pcm->OutboundTx)`).  Safe when no ILV exists at that slot yet
// (pilv[0] == nullptr before Stage D's create_ilv call site): the
// setter's resolve_ilv returns nullptr and the setter early-
// returns without effect.  Once Stage D ports the xmtr xcmaster
// pump to construct ILV at id=0, this call wires the operator-
// registered TX-out callback into xilv's Outbound dispatch
// automatically; no further plumbing change needed.
//
// Mirror of the SendpOutboundRx Stage B.5 wire-up at line 67
// above — identical hand-off discipline, identical reference
// pattern (cmaster.c:411 vs :418).

void SendpOutboundTx(OutboundCallback cb)
{
    pcm->OutboundTx = std::move(cb);
    lyra::wdsp::SetILVOutputPointer(0, pcm->OutboundTx);
}

// ===== SendpOutboundTCIRxIQ =====
//
// Reference cmaster.c:421-425.  TCI RX I/Q sample callback.  No
// downstream wire-up in the reference (the callback is invoked
// directly from the xcmaster pump's TCI dispatch site, which lands
// in Stage D when xcmaster body ports).

void SendpOutboundTCIRxIQ(OutboundCallback cb)
{
    pcm->OutboundTCIRxIQ = std::move(cb);
}

// ===== SendpInboundTCITxAudio =====
//
// Reference cmaster.c:428-432.  TCI TX audio callback (host →
// radio TCI audio injection).  No downstream wire-up — invoked
// from xcmaster pump's TX dispatch when `use_tci_audio` is set
// (cmaster.c:380-385); that path lands in Stage D.

void SendpInboundTCITxAudio(InboundCallback cb)
{
    pcm->InboundTCITxAudio = std::move(cb);
}

// ===== SetTCIRun =====
//
// Reference cmaster.c:434-437:
//   void SetTCIRun (int active) {
//       _InterlockedExchange (&pcm->tci_run, active);
//   }
//
// Lyra uses plain int assignment per the reference Rule 24
// source-verification finding that the field is read at use sites
// via `_InterlockedAnd` for fence semantics — write-side does NOT
// require atomic on x86_64 word-sized stores.  `volatile long` →
// plain `int` matches the same correction applied to XmitBit in
// §3.4 (RadioNet.h:716-727).

void SetTCIRun(int active)
{
    pcm->tci_run = active;
}

}  // namespace lyra::wire
