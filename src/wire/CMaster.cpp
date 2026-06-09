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
#include "wire/CmBuffs.h"   // Phase C — create_cmbuffs / destroy_cmbuffs / pcm_in
#include "wire/Router.h"
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

// =====================================================================
// create_cmaster — reference cmaster.c:273-320
// =====================================================================
//
// Ports the STRUCTURE of `create_cmaster` at the central app-startup
// lifecycle point.  Every Thetis subsystem-create is enumerated below
// (either as a real Lyra-cpp call or as a `// DEFERRED [where]` comment
// citing the exact reference line + the Lyra-cpp location of the
// equivalent work) so a future audit can grep + verify nothing was
// silently dropped from the reference scaffold.
//
// Reference body (cmaster.c:273-320), step-by-step:
//
//   line 276-286:  for each stream:
//                    InitializeCriticalSectionAndSpinCount
//                    create_cmbuffs(...)
//                    pcm->in[i] = malloc0(...)
//   line 287:      create_rcvr()
//   line 288:      create_xmtr()
//   line 289-314:  create_aamix(0, 0, ..., pcm->OutboundRx, ...)
//   line 315:      create_cmasio()
//   line 316:      create_router(0)
//   line 317:      pcm->panalalloc = create_analyzer_alloc(32, 40)
//
void create_cmaster()
{
    // -----------------------------------------------------------------
    // Reference cmaster.c:276-286 — per-stream init.
    //
    // PHASE C 2026-06-09: Thetis-faithful CMB ring port shipped (see
    // src/wire/CmBuffs.{h,cpp}).  Per-stream `create_cmbuffs(id, accept,
    // max_insize, max_outsize, outsize)` populates the elastic CMB ring
    // + semaphore + spawns the cm_main pump thread.  This block now
    // calls create_cmbuffs for stream id=1 (TX) — the only stream Lyra-
    // cpp v0.2.0 SSB drives through the central pump.  RX streams (0
    // and 2 in Thetis nddc=4) stay on the WdspEngine direct dispatch
    // path per the Lyra-cpp pre-existing RX architecture (see Stage E.0
    // audit + the create_rcvr defer comment below).
    //
    // Stream 1 (TX) parameter values per Thetis cmsetup.c posture for
    // HL2 SSB v0.2:
    //   * max_insize   = 256 complex samples — generous upper bound on
    //                    HL2 mic block size per EP6 datagram (typical
    //                    9-19 complex samples per Inbound call after
    //                    decimation to 48 kHz; 256 gives ~13× headroom
    //                    for any decimation-factor change).
    //   * max_outsize  = 64  complex samples — referenceBuffsize(48000)
    //                    = 64 per cmsetup.c:106-111 (= TxChannel's
    //                    inSize_ — see TxChannel.cpp).
    //   * outsize      = 64  complex samples — the cm_main pump's
    //                    per-iteration drain target == max_outsize.
    //   * accept       = 1   — open the Inbound() gate immediately
    //                    (HL2 mic is always-on; cmbuffs accepts
    //                    samples even when MOX=off, the TX-IQ-on-wire
    //                    gating happens downstream at the EP2 packer's
    //                    `injectTxIq_` flag).
    //
    // The ring is wire-quiescent until Stage 7.5 wires Hl2Ep6MicSource's
    // consumer to call lyra::wire::Inbound(1, n, mic_iq) — no producer
    // = semaphore never released = pump thread idle = no behavior
    // change.  xcmaster(1)'s case-1 body (below in this file) also
    // safely no-ops on empty pxmtr[0] until Stage 7.2-7.3 publishes
    // the TxChannel via create_xmtr_hl2.  Safe to land + run today.
    create_cmbuffs(/*id=*/1, /*accept=*/1,
                   /*max_insize=*/256,
                   /*max_outsize=*/64,
                   /*outsize=*/64);
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:287 — create_rcvr().
    //
    // DEFERRED [WdspEngine]:
    //   Lyra-cpp's RX path lives in `WdspEngine` (`src/dsp/WdspEngine.h`)
    //   — operator-constructed in `main.cpp` after `wdsp->load()`
    //   succeeds.  `WdspEngine::openRx1()` is the Lyra-cpp equivalent
    //   of Thetis `create_rcvr`'s WDSP RXA OpenChannel + per-RX-config
    //   path.  This is the Stage E.0 audit's documented architectural
    //   decision (RX lifecycle in WdspEngine, not in CMaster).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:288 — create_xmtr().
    //
    // DEFERRED [Stage 7.2-7.3, main.cpp QTimer block @ ~line 576]:
    //   Lyra-cpp's TX channel construction (`TxChannel`) + ILV setup
    //   (`create_ilv`) + per-xmtr bank publication (`create_xmtr_hl2`,
    //   in this file ~line 196) happen INSIDE the post-wdsp->load()
    //   QTimer block in main.cpp.  This split is forced by Lyra's
    //   runtime-loaded WDSP.dll (vs Thetis's static link) — TxChannel
    //   construction must defer until WDSP DLL symbols resolve.
    //   See `docs/architecture/STAGE_7_TX_WIRE_DESIGN.md` for the
    //   construction-site rationale.
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:289-314 — create_aamix(0, 0, ...) RX mixer.
    //
    // DEFERRED [Stage B AAMix port — already shipped]:
    //   Lyra-cpp's AAMix lives in `src/wdsp/AAMix.{h,cpp}` (Stage B
    //   port, shipped).  The RX-side create_aamix call at cmaster.c:
    //   297-313 is realized in Lyra by the `WdspEngine` constructor
    //   building its own AAMix instance at id=0 + wiring `pcm->
    //   OutboundRx` via `SetAAudioMixOutputPointer(nullptr, 0, cb)`
    //   inside `SendpOutboundRx` (this file, line 55).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:315 — create_cmasio() ASIO setup.
    //
    // DEFERRED [Stage E+ — no Lyra-cpp ASIO yet]:
    //   Lyra-cpp v0.2.x ships HL2 EP6 audio + AK4951 codec path only.
    //   ASIO is a future-stage item for non-HL2 hardware classes
    //   (per CLAUDE.md §15.12 + the Stage E+ ASIO entry).  Marker
    //   FIXMEs already exist in `RadioNet.cpp:267,296`.
    // -----------------------------------------------------------------

    // Reference cmaster.c:316 — create_router(0).
    //
    // This IS the call that previously lived in main.cpp:234 as the
    // bare wire-shell stand-in for create_cmaster().  Now it's where
    // the reference puts it — inside create_cmaster — so the call-
    // graph matches reference shape.  process-singleton Router slot 0
    // (the only Lyra-cpp router slot today; RX/TX share it).
    create_router(0);

    // -----------------------------------------------------------------
    // Reference cmaster.c:317 — pcm->panalalloc = create_analyzer_alloc(32, 40).
    //
    // DEFERRED [Stage E.1 — Task #140]:
    //   TX-side spectrum analyzer (separate from the RX panadapter
    //   analyzer Lyra already has in `WdspEngine`).  PureSignal v0.3
    //   prerequisite per Stage E.0 audit + the `calcc.c` IMD-
    //   measurement dependency.  Marker exists in `wdsp_engine.cpp`
    //   around line 775-788.  Until Stage E.1 ships, `pcm->panalalloc`
    //   stays nullptr; no Lyra-cpp code reads it yet.
    // -----------------------------------------------------------------
}

// =====================================================================
// destroy_cmaster — reference cmaster.c:322-337
// =====================================================================
//
// REVERSE-ORDER teardown of create_cmaster.  Mirrors:
//
//   line 325:      destroy_analyzer_alloc()
//   line 326:      destroy_router(0, 0)
//   line 327:      destroy_cmasio()
//   line 328:      destroy_aamix(0, 0)
//   line 329:      destroy_xmtr()
//   line 330:      destroy_rcvr()
//   line 331-336:  for each stream:
//                    DeleteCriticalSection
//                    destroy_cmbuffs(i)
//                    _aligned_free(pcm->in[i])
//
void destroy_cmaster()
{
    // -----------------------------------------------------------------
    // Reference cmaster.c:325 — destroy_analyzer_alloc().
    //
    // DEFERRED [Stage E.1 sibling — Task #140].
    // -----------------------------------------------------------------

    // Reference cmaster.c:326 — destroy_router(0, 0).
    //
    // Previously lived in main.cpp:445 handler-4 as the bare wire-shell
    // stand-in for destroy_cmaster().  Now inside destroy_cmaster
    // matching reference call-graph.  The two args are (id, variant);
    // both 0 for the Lyra-cpp single-router slot.
    destroy_router(0, 0);

    // -----------------------------------------------------------------
    // Reference cmaster.c:327 — destroy_cmasio().
    //
    // DEFERRED [Stage E+, no Lyra-cpp ASIO yet — sibling of the
    // create_cmasio defer above].
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:328 — destroy_aamix(0, 0).
    //
    // DEFERRED [Stage B sibling — owned by WdspEngine].
    //   Lyra-cpp's WdspEngine destructor handles its own AAMix(id=0)
    //   destruction symmetric to its construction.  No CMaster-level
    //   destroy_aamix needed (matches the "construction in WdspEngine"
    //   architectural decision).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:329 — destroy_xmtr().
    //
    // DEFERRED [Stage 7.x main.cpp aboutToQuit handler]:
    //   Lyra-cpp's TxChannel teardown (handler between handler-1 and
    //   handler-2 per the §11.5 v2 plan-paper) calls TxChannel::stop()
    //   + close() + destroy_xmtr_hl2() + destroy_ilv() in reverse
    //   construction order — matches reference destroy_xmtr posture.
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:330 — destroy_rcvr().
    //
    // DEFERRED [WdspEngine destructor — sibling of create_rcvr defer].
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:331-336 — per-stream teardown.
    //
    // PHASE C 2026-06-09: tears down the per-stream CMB ring + cm_main
    // pump thread + per-stream pcm->in[] buffer.  Reverse-order match
    // to create_cmaster's create_cmbuffs above.
    //
    // Stream 1 (TX) teardown.  destroy_cmbuffs() coordinates with the
    // pump thread (shuts Inbound gate → traps cm_main → joins the
    // jthread → cleans up) — see CmBuffs.cpp destroy_cmbuffs body.
    //
    // SHUTDOWN-ORDER RATIONALE:
    //   Lyra-cpp's aboutToQuit handler chain (main.cpp) runs in order:
    //     handler-1: stream->registerTx{IqSource,Control}({}) [TX cbs]
    //     handler-2: stream->close()                          [EP6 join]
    //     handler-3: delete micSource                         [mic-source dtor]
    //     handler-4: destroy_cmaster()                        [this code]
    //   By the time destroy_cmbuffs runs here:
    //     * EP6 thread is JOINED (handler-2) -> no more Inbound() calls
    //     * micSource is destroyed (handler-3) -> consumer registration
    //                                              has been cleared
    //   So the pump thread is idle (waiting on Sem_BuffReady with no
    //   producer); destroy_cmbuffs releases the sem + traps the thread
    //   + joins cleanly.  No race.
    destroy_cmbuffs(/*id=*/1);
    // -----------------------------------------------------------------
}

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
        // case 1 — standard transmitter.
        //
        // PHASE C 2026-06-09: real Thetis-faithful dispatch.  Per
        // cmaster.c:377-398 (the reference's case 1 body), the central
        // pump entry reads `pcm->in[stream]` (the per-stream input
        // buffer the cm_main pump drained from the CMB ring via cmdata
        // at cmbuffs.c:164) and calls fexchange0 + xilv + supporting
        // stages.
        //
        // Lyra-cpp's equivalent:
        //   * mic_in = pcm_in(stream)   — the cm_main pump just wrote
        //                                  r1OutSize complex samples
        //                                  here via cmdata() at
        //                                  CmBuffs.cpp:cm_main loop
        //   * n       = pcm_in_size(stream)
        //                                = r1OutSize set in
        //                                  create_cmbuffs (64 for
        //                                  HL2 SSB v0.2 = 48 kHz mic).
        //   * xmtr_id = txid_stub(stream) — stream 1 → xmtr 0 today.
        //
        // xcmasterTickTx does the reduced HL2-SSB-v0.2 subset of the
        // reference case-1 body: TxChannel.process (= fexchange0) +
        // xilv(pilv[0], outBuffers).  Deferred reference stages:
        //   * asioIN (cmaster.c:379)               — no Lyra ASIO yet
        //   * use_tci_audio override (380-386)     — TCI defer
        //   * xpipe (387)                          — Lyra-native sink
        //   * xdexp (388)                          — VOX defer (v0.2.3)
        //   * xsidetone (391)                      — CW defer (v0.2.2)
        //   * xpipe (392)                          — Lyra-native sink
        //   * xMixAudio monitor (394)              — monitor defer
        //   * xtxgain (395)                        — PS defer (v0.3)
        //   * xeer (396)                           — EER defer (HL2 N/A)
        //
        // Each deferred stage is documented in CMaster.cpp xcmasterTickTx
        // (or via Stage E.0 audit) — none block first-RF SSB.
        //
        // Wire-quiescent invariant: until Stage 7.2-7.3 publishes the
        // TxChannel via create_xmtr_hl2(0, tx_channel, ilv_xmtr_id),
        // pxmtr[0] is empty.  resolve_xmtr returns nullptr +
        // xcmasterTickTx early-returns safely (Stage D.2 unit-test
        // verified).  So even though the cm_main pump is alive in
        // Phase C, case 1 here no-ops cleanly until Stage 7.x wires
        // the consumer side.  No bench regression today; first-RF
        // becomes possible when Stage 7.x publishes pxmtr[0].
        const int tx     = txid_stub(stream);
        double*   mic_in = pcm_in(stream);
        const int n      = pcm_in_size(stream);
        if (mic_in && n > 0) {
            xcmasterTickTx(tx, mic_in, n);
        }
        // else: cmbuffs not yet created for this stream (impossible
        // if create_cmaster was called per the documented contract
        // — but defensive null-check matches the Lyra-native safer-
        // than-reference posture documented in resolve_xmtr).
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
