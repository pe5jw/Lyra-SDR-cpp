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
#include "wdsp/TxChannel.h"

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

// ===== Stage D — xcmaster pump body + pxmtr[] bank =====
//
// Reference cmaster.c:340-410 `PORT void xcmaster(int stream)`
// dispatches per-stream into RX (case 0) / TX (case 1) / special
// (case 2) bodies.  Lyra-cpp Stage D implements case 1 for HL2 SSB
// v0.2 minimum (per Stage E.0 audit findings — see
// docs/architecture/STAGE_E_TXCHANNEL_AUDIT.md).  Cases 0 and 2 are
// documented stubs.

// [Lyra-native] Process-wide xmtr bank.  See CMaster.h for the
// reference `pcm->xmtr[]` ↔ Lyra-native `pxmtr[]` sidestep rationale.
std::array<XmtrSlot, MAX_EXT_XMTR> pxmtr{};

namespace {
// Resolve an xmtr_id into the bank slot pointer.  Returns nullptr
// on out-of-range id OR empty slot — every caller early-returns on
// nullptr (matches the AAMix/ILV resolve_* discipline of safely
// no-op'ing rather than crashing on a not-yet-created transmitter;
// reference would deref null pcm->xmtr[id].pilv and crash, but the
// pre-rebuild wire-inert posture makes the safer no-op correct).
XmtrSlot* resolve_xmtr(int xmtr_id) noexcept
{
    if (xmtr_id < 0 || xmtr_id >= MAX_EXT_XMTR) {
        return nullptr;
    }
    XmtrSlot& slot = pxmtr[xmtr_id];
    if (slot.tx_channel == nullptr) {
        return nullptr;
    }
    return &slot;
}
} // namespace

// ===== create_xmtr_hl2 =====
//
// Lyra-cpp HL2 SSB minimum equivalent of reference
// cmaster.c:112-253 `create_xmtr()`.  The full reference allocates
// 9 per-xmtr surfaces; Stage E.0 audit established that 4 (out
// buffers / OpenChannel / ILV) ship and 5 are reference-`run=0`
// for HL2 or deferred-by-design.  Stage D's create_xmtr_hl2
// expects the caller to have already constructed those parts:
//   * TxChannel — handles out[0..2] allocation + OpenChannel.
//   * ILV slot id — caller invoked lyra::wdsp::create_ilv(...) with
//     run=0 (bypass mode for HL2 — cmaster.c:227), insize matching
//     TxChannel's outSize(), what=3 (matches reference but bypass
//     path ignores `what`), ninputs=2 (matches reference EER pair).
//
// This function just publishes the {tx_channel, ilv_xmtr_id} pair
// into pxmtr[xmtr_id] so xcmaster can route to them.  Out-of-range
// xmtr_id is a silent no-op (defensive; matches AAMix's id-bounds
// posture).

void create_xmtr_hl2(int xmtr_id,
                     lyra::wdsp::TxChannel* tx_channel,
                     int ilv_xmtr_id)
{
    if (xmtr_id < 0 || xmtr_id >= MAX_EXT_XMTR) {
        return;
    }
    pxmtr[xmtr_id] = XmtrSlot{tx_channel, ilv_xmtr_id};
}

// ===== destroy_xmtr_hl2 =====
//
// Clears the central pxmtr[] bank slot for xmtr_id, but only if the
// slot still references the caller-supplied tx_channel.  Defensive
// against the race where a newer create_xmtr_hl2 has already
// replaced the slot before this destroy ran (matches AAMix /
// ILV destroy_* discipline).  Does NOT destroy the TxChannel /
// ILV — caller owns those lifetimes.

void destroy_xmtr_hl2(int xmtr_id, lyra::wdsp::TxChannel* tx_channel)
{
    if (xmtr_id < 0 || xmtr_id >= MAX_EXT_XMTR) {
        return;
    }
    if (pxmtr[xmtr_id].tx_channel == tx_channel) {
        pxmtr[xmtr_id] = XmtrSlot{};
    }
}

// ===== xcmasterTickTx =====
//
// Lyra-cpp HL2 SSB pump body.  Direct port of reference
// cmaster.c:381-407 case 1 (TX), reduced to the HL2-essential
// surfaces per Stage E.0 audit:
//
// Reference case 1 (cmaster.c:381-407, paraphrased):
//   tx = txid(stream);
//   asioIN(pcm->in[stream]);                    // HL2 = no-op (ASIO off)
//   if (use_tci_audio) memcpy from TCI;         // DEFERRED (rebuild)
//   xpipe(stream, 0, pcm->in);                  // diagnostic tap (no-op)
//   xdexp(tx);                                  // VOX = Task #91 / v0.2.3
//   fexchange0(...);                            // *** REQUIRED ***
//   xsidetone(tx);                              // CW = v0.2.2
//   xpipe(stream, 1, ...);                      // diagnostic tap (no-op)
//   xMixAudio(0, 0, ..., out[2]);               // monitor mix (sidetone-tied)
//   xtxgain(pgain);                             // HL2 no Penelope (run=0)
//   xeer(peer);                                 // HL2 no EER (run=0)
//   xilv(pilv, out);                            // *** REQUIRED ***
//
// Lyra-cpp HL2 SSB equivalent: fexchange0 (via TxChannel::process)
// + xilv (via Stage C ported xilv).  Everything else is a reference-
// run=0 or deferred-feature surface that the reference also does
// not exercise for an HL2-class radio in SSB.

void xcmasterTickTx(int xmtr_id, double* mic_in, int n_samples)
{
    XmtrSlot* slot = resolve_xmtr(xmtr_id);
    if (slot == nullptr) {
        return;  // no transmitter configured; safe no-op
    }
    lyra::wdsp::TxChannel* tx = slot->tx_channel;

    // 1. fexchange0 — mic IQ → TXA chain → TX I/Q output (out[0]).
    //    Reference cmaster.c:389:
    //      fexchange0(chid(stream, 0), pcm->in[stream],
    //                 pcm->xmtr[tx].out[0], &error);
    //    TxChannel::process() encapsulates the equivalent call;
    //    the return code mirrors reference's `error` (0 = success).
    //    Currently no consumer for the error code beyond the bypass
    //    semantics — drops a stale-pre-channel-open block silently
    //    (matches reference posture for an !opened_ channel).
    const int err = tx->process(mic_in, n_samples);
    (void) err;  // No upstream handler today; matches reference posture
                 // (reference passes &error but doesn't act on its value
                 // either — it falls through to xilv unconditionally).

    // 2. xilv — interleave (bypass mode for HL2: copies out[0] into
    //    outbuff) → dispatch through ILV.Outbound → operator-registered
    //    SendpOutboundTx callback → EP2 wire.
    //    Reference cmaster.c:407:
    //      xilv(pcm->xmtr[tx].pilv, pcm->xmtr[tx].out);
    //    The pcm->xmtr[tx].out 3-ptr array is Lyra's
    //    TxChannel::outBuffers() (Stage D.1 API extension).
    auto bufs = tx->outBuffers();
    lyra::wdsp::ILV* ilv = lyra::wdsp::pilv[
        static_cast<std::size_t>(slot->ilv_xmtr_id)];
    if (ilv != nullptr) {
        lyra::wdsp::xilv(ilv, bufs.data());
    }
    // If the ILV slot is empty (caller skipped create_ilv) the xilv
    // call would deref nullptr — early-return is the safer
    // pre-rebuild posture matching resolve_xmtr's nullptr-safety.
}

// ===== xcmaster (reference cmaster.c:340) =====
//
// Reference `PORT void xcmaster(int stream)`.  Dispatches per-stream
// type: case 0 = standard receiver, case 1 = standard transmitter,
// case 2 = special upper-panadapter stitch.
//
// Lyra-cpp Stage D ports the full signature + 3-case switch
// structure even though only case 1 has a body.  Cases 0 and 2 are
// documented stubs — case 0 because Lyra dispatches RX via
// WdspEngine in its pre-existing architecture (separate from this
// central pump); case 2 because the special-panadapter stitch is
// not used.  Future-stage RX migration into the central pump (if/
// when the architecture warrants) plugs into the existing switch
// without signature churn — matches the reference's growth pattern.
//
// In the reference, `stream` is the cmaster stream id (one per RX
// or TX in the multi-channel scheme).  Lyra-cpp's single-transmitter
// v0.2 maps `stream` to a stype()/txid() pair via a stub helper
// (matches the reference convention even though the dispatch
// produces a single target — the stub here documents the parity).
//
// xcmaster is the caller-facing entry point the consumer thread
// (TX pump in the rebuild's wire-layer rebuild) is expected to
// call once per mic-input block.  The rebuild invokes it with the
// stream id that maps to the active transmitter; xcmaster's case
// 1 body dispatches to xcmasterTickTx for the actual DSP +
// dispatch work.
//
// Pre-rebuild, no production caller exists for xcmaster — Stage D
// ships the function ready for the rebuild's consumer-side wire-up
// (currently in_progress per Task #112/#117).

namespace {
// [Lyra-native] stub stream-type helper.  Reference cmaster.c uses
// stype(stream) reading from a global stream-type table populated
// at create_cmaster time.  Lyra-cpp's single-transmitter v0.2 maps
// the single TX stream (id 0) to type 1 (TX) and reserves the
// pattern for RX migration (would return 0 for an RX stream id if
// the central pump grows to handle RX).
int stype_stub(int stream) noexcept
{
    // Stage D minimal: stream 0 = TX (matches reference HL2
    // convention where the single TX stream is stream 0 for
    // the single-transmitter case).  Future-stage RX migration
    // expands this to map RX stream ids to type 0.
    return (stream == 0) ? 1 : -1;
}

// [Lyra-native] stub txid helper.  Reference txid(stream) returns
// the per-xmtr id within the TX-streams subset.  v0.2 single
// transmitter → always 0 for a TX-typed stream.
int txid_stub(int /*stream*/) noexcept
{
    return 0;
}
} // namespace

void xcmaster(int stream)
{
    switch (stype_stub(stream)) {
    case 0:
        // case 0 — standard receiver.  Reference cmaster.c:347-373
        // body does xpipe / xanb / xnob / Spectrum0 / fexchange0 /
        // xMixAudio for the per-RX path.
        //
        // Lyra-cpp dispatches RX via WdspEngine in its pre-existing
        // architecture (separate from this central pump).  Stub
        // preserves reference shape for future-stage RX migration
        // if/when warranted.
        break;
    case 1: {
        // case 1 — standard transmitter.  Stage D HL2 SSB body
        // dispatches to xcmasterTickTx for the fexchange0 + xilv
        // work.  Mic input comes from the consumer thread's caller
        // context (not the reference's pcm->in[stream] central
        // buffer — see CMaster.h xcmasterTickTx docstring for the
        // explicit-parameter Lyra-native idiom translation).
        //
        // xcmaster(stream) itself is the no-mic-passing variant for
        // reference signature parity.  The active TX pump entry is
        // xcmasterTickTx(xmtr_id, mic_in, n).  Until the rebuild
        // wires a caller, this branch is reached only by direct
        // unit tests (which exercise xcmasterTickTx directly with
        // explicit mic_in).
        const int tx = txid_stub(stream);
        (void) tx;
        // Intentional no-op for the bare xcmaster() call -- the
        // mic-input data has to come from somewhere, and Stage D
        // expects callers that have mic data to use xcmasterTickTx
        // directly (operator-explicit data ownership).
        break;
    }
    case 2:
        // case 2 — special upper-panadapter stitch.  Reference
        // cmaster.c:402-404 body does xpipe(stream, 0, pcm->in) only.
        // Not used in Lyra-cpp's pre-existing architecture; stub
        // preserves reference shape for completeness.
        break;
    default:
        // Unknown stream type — silent no-op (matches reference
        // posture of an unmatched switch case).
        break;
    }
}

}  // namespace lyra::wire
