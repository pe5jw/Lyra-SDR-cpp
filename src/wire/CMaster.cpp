// Lyra — ChannelMaster (cmaster) layer implementation.  See CMaster.h
// for the multi-stage port plan + reference-mapping rationale.
//
// Stage A (this commit): module shell.  cmaster struct typedef +
// global pcm pointer + enum AudioCodecId + 4 SendpOutbound* setter
// stubs.  NO behaviour change — Stage A populates the architectural
// shell that subsequent stages (aamix port, ilv port, xcmaster pump
// body, etc.) plug into.

#include "wire/CMaster.h"

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
    // Stage B PENDING: SetAAudioMixOutputPointer(0, 0, pcm->OutboundRx);
}

// ===== SendpOutboundTx =====
//
// Reference cmaster.c:414-419:
//   void SendpOutboundTx(void (*Outbound)(int id, int nsamples, double* buff)) {
//       pcm->OutboundTx = Outbound;
//       SetILVOutputPointer(0, pcm->OutboundTx);
//   }
//
// Stage A: stores callback only.  The
// `SetILVOutputPointer(0, pcm->OutboundTx)` call lands when Stage C
// (ilv port) ships.

void SendpOutboundTx(OutboundCallback cb)
{
    pcm->OutboundTx = std::move(cb);
    // Stage C PENDING: SetILVOutputPointer(0, pcm->OutboundTx);
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
