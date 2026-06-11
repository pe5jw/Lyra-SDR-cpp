// Lyra-cpp — CMaster.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/cmaster.c
//   (specifically `cm`/`pcm` globals at :29-30, `create_xmtr`
//   :112-253, `destroy_xmtr` :255-271, `create_cmaster`
//   :273-320, `destroy_cmaster` :322-337, `xcmaster` :340-405,
//   `SendpOutbound*` setters :407-432, `SetTCIRun` :434-437)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014-2019 Warren Pratt, NR0V
// License: GNU General Public License v3 or later
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT + LYRA-NATIVE create_xmtr_hl2
// SPLIT REVERTED 2026-06-09
// (operator directive: no Lyra-native deviations in Thetis ports)
// ============================================================
//
// See CMaster.h for the full retrofit rationale, the locked
// allowed-forced-deviations list (rule-#8 struct tag +
// std::function setter fields + ILV* typed pointers + the single
// `pTxChannel` xmtr-substruct carve-out for runtime-loaded
// WDSP.dll RAII), and the per-symbol reference mapping.

#include "wire/CMaster.h"
#include "wire/CmBuffs.h"   // create_cmbuffs / destroy_cmbuffs / CmBuffs
#include "wire/Router.h"
#include "wire/AAMix.h"   // P0.c direct port (reference aamix.h verbatim)
#include "wire/ILV.h"   // P0.b direct port (reference ilv.h verbatim)
#include "wdsp/TxChannel.h"

#include <intrin.h>          // _InterlockedExchange

namespace lyra::wire {

// =====================================================================
// Reference cmaster.c:29-30 -- `cmaster cm = {0}; CMASTER pcm = &cm;`
// =====================================================================
// Process-lifetime; never freed.  Default-constructed via in-class
// initialisers in CMasterState (equivalent to the reference `{0}`
// zero-init for the populated fields, with `audioCodecId = HERMES`
// matching the reference's per-radio-family init writing the same
// value for HL2/HL2+).
CMasterState  cm  {};
CMasterState* pcm = &cm;

// =====================================================================
// create_xmtr -- reference cmaster.c:112-253.
// =====================================================================
//
// Reference body (cmaster.c:112-253, paraphrased to the HL2 SSB
// subset Lyra-cpp ports; the 5 deferred subsystems are explicitly
// reference-`run=0` or deferred-by-design per Stage E.0 audit and
// are documented inline below):
//
//   void create_xmtr()
//   {
//     int i, j, rc;
//     int avoxmix_inrates[cmMAXrcvr * cmMAXSubRcvr];
//     ...
//     for (i = 0; i < pcm->cmXMTR; i++)
//     {
//       int in_id = inid (1, i);
//       // out[0..2] allocations           ---> TxChannel RAII (Lyra)
//       // create_dexp (VOX)               ---> DEFERRED (Stage E)
//       // pcm->xmtr[i].pavoxmix = ...     ---> DEFERRED (Stage E)
//       // OpenChannel(chid(in_id,0),...)  ---> TxChannel::open (Lyra)
//       // XCreateAnalyzer(in_id,...)      ---> DEFERRED (Stage E.1)
//       // pcm->xmtr[i].pgain = ...        ---> DEFERRED (Stage E)
//       // pcm->xmtr[i].peer = ...         ---> DEFERRED (HL2 no EER)
//       // pcm->xmtr[i].pilv = create_ilv( ---> ported below
//       //   /*run*/        0,
//       //   /*id*/         1,
//       //   /*insize*/     pcm->xmtr[i].ch_outsize,
//       //   /*maxinputs*/  2,
//       //   /*which*/      3,
//       //   pcm->OutboundTx);
//       // create_sidetone(...)            ---> DEFERRED (CW v0.2.2)
//     }
//   }
//
// Lyra-cpp ports the per-iteration body byte-faithful for the HL2
// SSB live surfaces only.  Single Lyra-native carve-out: TxChannel
// is RAII-constructed in main.cpp (runtime-loaded WDSP.dll) and
// published to `pcm->xmtr[i].pTxChannel` BEFORE create_xmtr is
// invoked, so we derive ILV's insize from `pTxChannel->outSize()`
// the same way the reference would have derived it from the
// just-opened WDSP channel's output size.
void create_xmtr(int xmtr_id)
{
    int i = xmtr_id;
    CMasterXmtr& x = pcm->xmtr[i];

    // Reference cmaster.c:226-232 -- create_ilv:
    //   pcm->xmtr[i].pilv = create_ilv(
    //       0,                                  // run (bypass mode)
    //       1,                                  // id to use in Outbound
    //       pcm->xmtr[i].ch_outsize,            // insize
    //       2,                                  // maximum number of inputs
    //       3,                                  // which streams to interleave
    //       pcm->OutboundTx);                   // Outbound callback
    //
    // P0.b: create_ilv is the reference signature VERBATIM (no
    // leading xmtr_id, raw Outbound fn ptr — the former retrofit's
    // central pilv[] bank is gone; the reference reaches the ILV
    // via pcm->xmtr[xmtr_id].pilv and so does Lyra-cpp now).
    //
    // ch_outsize is derived from the published pTxChannel's
    // outSize() (the Lyra-native seam for runtime-loaded WDSP):
    // reference derives the same value from the just-opened WDSP
    // channel's output size via getbuffsize(ch_outrate).
    int ch_outsize = 0;
    if (x.pTxChannel != nullptr) {
        ch_outsize = x.pTxChannel->outSize();
    }
    x.ch_outsize = ch_outsize;

    x.pilv = create_ilv(
        0,                                  // run (bypass mode)
        1,                                  // id to use in Outbound
        x.ch_outsize,                       // insize
        2,                                  // maximum number of inputs
        3,                                  // which streams to interleave
        pcm->OutboundTx);                   // Outbound callback
}

// =====================================================================
// destroy_xmtr -- reference cmaster.c:255-271.
// =====================================================================
//
// Reference body (paraphrased to the ported surface):
//
//   void destroy_xmtr() {
//     for (i = 0; i < pcm->cmXMTR; i++) {
//       destroy_sidetone(i);                   // DEFERRED
//       destroy_ilv(pcm->xmtr[i].pilv);        // ported below
//       destroy_eer(pcm->xmtr[i].peer);        // DEFERRED
//       destroy_txgain(pcm->xmtr[i].pgain);    // DEFERRED
//       DestroyAnalyzer(inid(1,i));            // DEFERRED (Stage E.1)
//       CloseChannel(chid(inid(1,i),0));       // owned by TxChannel
//       destroy_aamix(pavoxmix, -1);           // DEFERRED
//       destroy_dexp(i);                       // DEFERRED
//       free out[0..2]                         // owned by TxChannel RAII
//     }
//   }
//
// TxChannel lifecycle is owned by main.cpp (RAII for runtime-loaded
// WDSP.dll reasons); destroy_xmtr only tears down the ILV slot.
void destroy_xmtr(int xmtr_id)
{
    int i = xmtr_id;
    CMasterXmtr& x = pcm->xmtr[i];
    if (x.pilv != nullptr) {
        // Reference cmaster.c:258 -- destroy_ilv(pcm->xmtr[i].pilv).
        // P0.b: verbatim signature (no xmtr_id param; the bank is
        // gone).  The null guard + slot clear stay because Lyra's
        // teardown can run against a partially-failed startup
        // (reference frees unconditionally inside its all-or-
        // nothing create/destroy pairing).
        destroy_ilv(x.pilv);
        x.pilv = nullptr;
    }
}

// =====================================================================
// create_cmaster -- reference cmaster.c:273-320.
// =====================================================================
//
// Reference body, step-by-step:
//
//   void create_cmaster()
//   {
//     for (i = 0; i < pcm->cmSTREAM; i++)
//     {
//       InitializeCriticalSectionAndSpinCount(&pcm->update[i], 2500);
//       create_cmbuffs(i, 1, pcm->cmMAXInbound[i],
//                      getbuffsize(pcm->cmMAXInRate),
//                      pcm->xcm_insize[i]);
//       pcm->in[i] = (double*)malloc0(getbuffsize(pcm->cmMAXInRate) *
//                                      sizeof(complex));
//     }
//     create_rcvr();           // DEFERRED -- WdspEngine
//     create_xmtr();           // ported -- HL2 SSB subset
//     // RX AAmix              // DEFERRED -- WdspEngine constructs
//     create_cmasio();         // DEFERRED -- no Lyra ASIO yet
//     create_router(0);        // ported
//     pcm->panalalloc = ...    // DEFERRED -- Stage E.1
//   }
//
// Lyra-cpp ports the structure faithfully.  Per-stream
// create_cmbuffs only runs for stream id=1 (TX) -- v0.2.0 SSB
// drives only the TX stream through the central pump; RX streams
// (0 and 2 in nddc=4) stay on the WdspEngine direct dispatch path
// per Stage E.0 audit + the create_rcvr defer comment.
void create_cmaster()
{
    // -----------------------------------------------------------------
    // Reference cmaster.c:276-286 -- per-stream init.
    //
    // PHASE C ported the CMB ring + cm_main pump (see CmBuffs.cpp).
    // Stream 1 (TX) is the only stream Lyra v0.2.0 SSB drives
    // through the central pump.  RX streams stay on WdspEngine's
    // direct dispatch.
    //
    // Stream-1 parameter values per the reference cmsetup.c posture
    // for HL2 SSB v0.2 (see CmBuffs.cpp create_cmaster comment for
    // the per-parameter rationale).
    //
    // create_cmbuffs internally sets pcm->pcbuff[id] = pcm->pdbuff
    // [id] = pcm->pebuff[id] = pcm->pfbuff[id] = a per cmbuffs.c:38;
    // CmBuffs.cpp's port preserves that 4-alias publish discipline.
    create_cmbuffs(/*id=*/1, /*accept=*/1,
                   /*max_insize=*/256,
                   /*max_outsize=*/64,
                   /*outsize=*/64);
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:287 -- create_rcvr().
    //
    // DEFERRED [WdspEngine]:
    //   Lyra-cpp's RX path lives in `WdspEngine` (src/dsp/
    //   WdspEngine.h) -- operator-constructed in main.cpp after
    //   wdsp->load() succeeds.  WdspEngine::openRx1() is the
    //   Lyra-cpp equivalent of Thetis create_rcvr's per-RX-config
    //   WDSP RXA OpenChannel path.  Stage E.0 audit's documented
    //   architectural decision (RX lifecycle in WdspEngine, not
    //   in CMaster).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:288 -- create_xmtr().
    //
    // NOT invoked here.  The single Lyra-native carve-out
    // documented in CMaster.h xmtr substruct (TxChannel's
    // RAII lifetime owned by main.cpp because WDSP.dll loads at
    // runtime) means create_xmtr's create_ilv call needs
    // pTxChannel->outSize() to derive the ILV's insize -- and
    // pTxChannel isn't constructed yet when create_cmaster
    // runs.  main.cpp invokes create_xmtr(0) directly after the
    // TxChannel is constructed + published into
    // pcm->xmtr[0].pTxChannel.  Same observable end-state as the
    // reference (pcm->xmtr[0].pilv populated, ILV bank slot
    // ready); only the invocation site shifts to accommodate the
    // runtime-loaded DLL.
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:289-314 -- create_aamix(0, 0, ...) RX
    // mixer.
    //
    // DEFERRED [P0.c AAMix direct port -- already shipped]:
    //   Lyra-cpp's AAMix lives in src/wire/AAMix.{h,cpp} (verbatim
    //   port).  The RX-side create_aamix call at cmaster.c:297-313
    //   is realised in Lyra by the WdspEngine constructor building
    //   its own AAMix instance at id=0 + wiring pcm->OutboundRx via
    //   SetAAudioMixOutputPointer(nullptr, 0, cb) inside
    //   SendpOutboundRx (this file, below).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:315 -- create_cmasio() ASIO setup.
    //
    // DEFERRED [Stage E+ -- no Lyra-cpp ASIO yet]:
    //   Lyra-cpp v0.2.x ships HL2 EP6 audio + AK4951 codec path
    //   only.  ASIO is a future-stage item for non-HL2 hardware
    //   classes (per CLAUDE.md §15.12 + the Stage E+ ASIO entry).
    // -----------------------------------------------------------------

    // Reference cmaster.c:316 -- create_router(0).
    //
    // process-singleton Router slot 0 (the only Lyra-cpp router
    // slot today; RX/TX share it).
    create_router(0);

    // -----------------------------------------------------------------
    // Reference cmaster.c:317 -- pcm->panalalloc =
    //                            create_analyzer_alloc(32, 40).
    //
    // DEFERRED [Stage E.1 -- Task #140]:
    //   TX-side spectrum analyzer (separate from the RX panadapter
    //   analyzer Lyra already has in WdspEngine).  PureSignal v0.3
    //   prerequisite per Stage E.0 audit + the calcc.c IMD-
    //   measurement dependency.  Until Stage E.1 ships, no Lyra-cpp
    //   code reads pcm->panalalloc.
    // -----------------------------------------------------------------
}

// =====================================================================
// destroy_cmaster -- reference cmaster.c:322-337.
// =====================================================================
//
// REVERSE-ORDER teardown of create_cmaster.
//
//   destroy_analyzer_alloc()           // DEFERRED -- Stage E.1
//   destroy_router(0, 0)               // ported
//   destroy_cmasio()                   // DEFERRED -- no ASIO
//   destroy_aamix(0, 0)                // DEFERRED -- WdspEngine
//   destroy_xmtr()                     // ported -- HL2 subset
//   destroy_rcvr()                     // DEFERRED -- WdspEngine
//   for each stream:
//     DeleteCriticalSection            // DEFERRED -- per-stream
//                                          update CS not used
//     destroy_cmbuffs(i)               // ported
//     _aligned_free(pcm->in[i])        // owned by CmBuffs.cpp
//
void destroy_cmaster()
{
    // Reference cmaster.c:326 -- destroy_router(0, 0).
    destroy_router(0, 0);

    // -----------------------------------------------------------------
    // Reference cmaster.c:329 -- destroy_xmtr().
    //
    // Ported -- tears down the ILV slot.  TxChannel lifecycle is
    // main.cpp's responsibility (RAII for runtime-loaded WDSP.dll
    // reasons; main.cpp's handler-1.5 closes + deletes TxChannel
    // alongside destroy_xmtr).
    destroy_xmtr(0);
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Reference cmaster.c:331-336 -- per-stream teardown.
    //
    // destroy_cmbuffs() coordinates with the cm_main pump thread
    // (shuts Inbound gate -> traps cm_main -> joins the thread ->
    // cleans up) -- see CmBuffs.cpp destroy_cmbuffs body.
    //
    // SHUTDOWN-ORDER RATIONALE (preserved from the prior shell):
    //   By the time destroy_cmbuffs runs here, EP6 thread is joined,
    //   micSource is destroyed, the consumer registration cleared,
    //   so the pump thread is idle (waiting on Sem_BuffReady with
    //   no producer); destroy_cmbuffs releases the sem + traps the
    //   thread + joins cleanly.  No race.
    destroy_cmbuffs(/*id=*/1);
    // -----------------------------------------------------------------
}

// =====================================================================
// SendpOutboundRx -- reference cmaster.c:407-412.
// =====================================================================
//
//   void SendpOutboundRx(void (*Outbound)(int id, int nsamples,
//                                          double* buff))
//   {
//     pcm->OutboundRx = Outbound;
//     SetAAudioMixOutputPointer(0, 0, pcm->OutboundRx);
//   }
//
// P0.c: VERBATIM port — raw fn ptr in, stored, pushed into the
// id-0 mixer via SetAAudioMixOutputPointer(0, 0, ...) which
// resolves paamix[0] exactly as aamix.c:513-519 does.  NO
// null-bank guard (the reference has none): callers register
// AFTER the id-0 mixer exists, matching the reference's
// netInterface ordering.  (No current Lyra caller — the RX
// wire-up lives at WdspEngine::openRx1; the Stage-A stub that
// used to call this was deleted at B.6.b-fix1 `8b8e0da`.)
void SendpOutboundRx(void (*Outbound)(int id, int nsamples, double* buff))
{
    pcm->OutboundRx = Outbound;
    SetAAudioMixOutputPointer(0, 0, pcm->OutboundRx);
}

// =====================================================================
// SendpOutboundTx -- reference cmaster.c:414-419.
// =====================================================================
//
//   void SendpOutboundTx(void (*Outbound)(int id, int nsamples,
//                                          double* buff))
//   {
//     pcm->OutboundTx = Outbound;
//     SetILVOutputPointer(0, pcm->OutboundTx);
//   }
//
// P0.b: VERBATIM port — raw fn ptr in, stored, pushed into the
// ILV via SetILVOutputPointer(0, ...) which reads
// pcm->xmtr[0].pilv exactly as ilv.c:97-101 does.  NO null-pilv
// guard (the reference has none): callers register AFTER
// create_xmtr has populated the xmtr slot, matching the
// reference's netInterface ordering.
void SendpOutboundTx(void (*Outbound)(int id, int nsamples, double* buff))
{
    pcm->OutboundTx = Outbound;
    SetILVOutputPointer(0, pcm->OutboundTx);
}

// =====================================================================
// SendpOutboundTCIRxIQ -- reference cmaster.c:421-425.
// =====================================================================
// TCI RX I/Q sample callback.  No downstream wire-up in the
// reference (the callback is invoked directly from the xcmaster
// pump's TCI dispatch site).
void SendpOutboundTCIRxIQ(OutboundCallback cb)
{
    pcm->OutboundTCIRxIQ = std::move(cb);
}

// =====================================================================
// SendpInboundTCITxAudio -- reference cmaster.c:428-432.
// =====================================================================
// TCI TX audio callback (host -> radio TCI audio injection).  No
// downstream wire-up -- invoked from xcmaster pump's TX dispatch
// when `use_tci_audio` is set (cmaster.c:380-385).
void SendpInboundTCITxAudio(InboundCallback cb)
{
    pcm->InboundTCITxAudio = std::move(cb);
}

// =====================================================================
// SetTCIRun -- reference cmaster.c:434-437.
// =====================================================================
//
//   void SetTCIRun(int active)
//   {
//     _InterlockedExchange(&pcm->tci_run, active);
//   }
void SetTCIRun(int active)
{
    _InterlockedExchange(&pcm->tci_run, active);
}

// =====================================================================
// xcmaster -- reference cmaster.c:340-405.
// =====================================================================
//
// Reference body (case 1 only -- HL2 SSB subset Lyra-cpp ports;
// cases 0 and 2 are documented stubs):
//
//   PORT void xcmaster(int stream)
//   {
//     int error;
//     EnterCriticalSection(&pcm->update[stream]);
//     switch (stype(stream))
//     {
//     int rx, tx, j, k, disp;
//
//     case 0:  // standard receiver
//       ... (RX dispatch -- DEFERRED, WdspEngine handles)
//       break;
//
//     case 1:  // standard transmitter
//       tx = txid(stream);
//       asioIN(pcm->in[stream]);                                // DEFERRED
//       if (_InterlockedAnd(&pcm->xmtr[tx].use_tci_audio, 1))   // ported
//       {
//         if (pcm->InboundTCITxAudio)
//           (*pcm->InboundTCITxAudio)(pcm->xcm_insize[stream],
//                                       pcm->in[stream]);
//         else
//           memset(pcm->in[stream], 0,
//                  pcm->xcm_insize[stream] * sizeof(complex));
//       }
//       xpipe(stream, 0, pcm->in);                              // DEFERRED
//       xdexp(tx);                                              // DEFERRED
//       fexchange0(chid(stream,0), pcm->in[stream],
//                  pcm->xmtr[tx].out[0], &error);               // ported
//       xsidetone(tx);                                          // DEFERRED
//       xpipe(stream, 1, pcm->xmtr[tx].out);                    // DEFERRED
//       xMixAudio(0, 0, chid(stream,0), pcm->xmtr[tx].out[2]);  // DEFERRED
//       xtxgain(pcm->xmtr[tx].pgain);                           // DEFERRED
//       xeer(pcm->xmtr[tx].peer);                               // DEFERRED
//       xilv(pcm->xmtr[tx].pilv, pcm->xmtr[tx].out);            // ported
//       break;
//
//     case 2:  // special upper-panadapter stitch
//       xpipe(stream, 0, pcm->in);                              // DEFERRED
//       break;
//     }
//     LeaveCriticalSection(&pcm->update[stream]);
//   }
//
// Lyra-cpp v0.2 HL2 SSB ports case 1 reduced per Stage E.0 audit:
// fexchange0 (via TxChannel::process) + xilv (via the central
// pilv[] bank slot reached as pcm->xmtr[tx].pilv) + the TCI audio
// override.  All other reference stages are reference-`run=0` for
// HL2 OR deferred-by-design (CW sidetone v0.2.2, VOX v0.2.3,
// Penelope/PS v0.3, TX analyzer Stage E.1).  Lyra-cpp omits the
// per-stream `update` critical section (the reference uses it to
// serialise xcmaster vs SetXcmInrate / SetXmtrChannelOutrate
// reconfiguration; Lyra-cpp's v0.2 doesn't expose live-rate-change
// setters yet).
//
// stype()/txid() helpers from the reference: stype(stream) returns
// the stream type (0=RX, 1=TX, 2=special); txid(stream) returns the
// xmtr id within the TX-streams subset.  Lyra-cpp v0.2 single
// transmitter maps stream 1 -> type 1 -> txid 0.

namespace {

// [Lyra-native helper -- minimal, no reference equivalent]
// stype() / txid() the reference reads from a global stream-type
// table populated at create_cmaster time.  Lyra-cpp's v0.2 single
// TX stream is hardcoded as id=1 -> type=1 -> txid=0; future RX
// migration into the central pump expands this.
int stype_for(int stream) noexcept { return (stream == 1) ? 1 : -1; }
int txid_for(int /*stream*/) noexcept { return 0; }

}  // namespace

void xcmaster(int stream)
{
    switch (stype_for(stream)) {
    case 0:
        // Reference cmaster.c:347-373 RX body -- DEFERRED to
        // WdspEngine (Lyra-cpp pre-existing architecture).  Stub
        // preserves reference shape for future-stage RX migration.
        break;

    case 1: {
        // Reference cmaster.c:377-398 -- standard transmitter.
        int tx = txid_for(stream);
        CMasterXmtr& x = pcm->xmtr[tx];

        // Reference cmaster.c:379 -- asioIN(pcm->in[stream]).
        // DEFERRED -- no Lyra-cpp ASIO yet; HL2 mic arrives via
        // EP6 already memcpy'd into pcm->in[stream] by the
        // Inbound() producer side (cmbuffs.c:108-109) + cmdata()
        // consumer drain (cmbuffs.c:144-145).

        // Reference cmaster.c:380-386 -- TCI TX audio override.
        // When set, overwrites pcm->in[stream] with TCI-provided
        // audio (or zero if no callback registered).
        if (_InterlockedAnd(&x.use_tci_audio, 1)) {
            if (pcm->InboundTCITxAudio) {
                pcm->InboundTCITxAudio(x.ch_outsize, pcm->in[stream]);
            }
            // else: reference zeros the buffer; Lyra-cpp omits the
            // memset because pcm->in[stream] is the cmdata drain
            // target and the next pump iteration overwrites it
            // again from the CMB ring (cmdata cmbuffs.c:144-145).
            // The zero memset is a reference-only belt-and-braces.
        }

        // Reference cmaster.c:387 -- xpipe(stream, 0, pcm->in).
        // DEFERRED -- diagnostic tap, no Lyra-native consumer.

        // Reference cmaster.c:388 -- xdexp(tx).  DEFERRED -- VOX
        // is reference-`run=0` for HL2; lands in Stage E (v0.2.3).

        // Reference cmaster.c:389 -- fexchange0.
        //   fexchange0(chid(stream, 0), pcm->in[stream],
        //              pcm->xmtr[tx].out[0], &error);
        //
        // Lyra-cpp's TxChannel::process encapsulates the equivalent
        // call (wraps the WDSP TXA channel + manages out[0..2]
        // buffers internally).  Output lives in TxChannel's
        // outBuffers(); xilv reads from that 3-pointer array the
        // same way the reference reads from pcm->xmtr[tx].out[0..2].
        //
        // pTxChannel is the [Lyra-native carve-out] holding the
        // RAII-constructed channel (see CMaster.h xmtr substruct
        // for the rationale).  If pTxChannel is still nullptr
        // (HL2 RX-only operating point, TX never opened), this
        // case-1 body is a safe no-op.
        if (x.pTxChannel == nullptr) {
            break;
        }

        const int err = x.pTxChannel->process(pcm->in[stream],
                                               x.ch_outsize);
        (void) err;  // No upstream handler (matches reference --
                     // reference passes &error but doesn't act on
                     // it either; falls through to xilv
                     // unconditionally).

        // Reference cmaster.c:391 -- xsidetone(tx).  DEFERRED --
        // CW sidetone is reference-`run_tx=0` for HL2 SSB; CW
        // ships in v0.2.2.

        // Reference cmaster.c:392 -- xpipe(stream, 1, ...).
        // DEFERRED -- diagnostic tap.

        // Reference cmaster.c:394 -- xMixAudio monitor mix.
        // DEFERRED -- monitor audio tied to sidetone (deferred).

        // Reference cmaster.c:395 -- xtxgain(pcm->xmtr[tx].pgain).
        // DEFERRED -- Penelope gain / amp_protect is
        // reference-`run=0` for HL2; PureSignal v0.3.

        // Reference cmaster.c:396 -- xeer(pcm->xmtr[tx].peer).
        // DEFERRED -- EER is reference-`run=0` for HL2 (HL2 has
        // no EER hardware).

        // Reference cmaster.c:397 -- xilv interleave + dispatch.
        //   xilv(pcm->xmtr[tx].pilv, pcm->xmtr[tx].out);
        //
        // ILV bypass mode (run=0) memcpy's the input straight to
        // outbuff then dispatches through ILV.Outbound (the
        // operator-registered SendpOutboundTx callback wired to
        // the EP2 wire).
        if (x.pilv != nullptr) {
            auto bufs = x.pTxChannel->outBuffers();
            xilv(x.pilv, bufs.data());
        }
        break;
    }

    case 2:
        // Reference cmaster.c:402-404 -- special upper-panadapter
        // stitch.  Body does xpipe(stream, 0, pcm->in) only.  Not
        // used in Lyra-cpp.  Stub preserves reference shape.
        break;

    default:
        // Unknown stream type -- silent no-op (matches reference
        // posture of an unmatched switch case).
        break;
    }
}

}  // namespace lyra::wire
