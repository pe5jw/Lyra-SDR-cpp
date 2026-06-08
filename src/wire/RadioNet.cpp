// Lyra — radio-network state container.  See RadioNet.h.
//
// Phase 2 step 13 — populated per the signed §1 parity checkpoint
// (docs/architecture/PARITY_CHECKPOINTS.md).  WIRE-INERT: built
// but not wired into HL2Stream until §10.3 step 14.

#include "wire/RadioNet.h"
#include "wire/CMaster.h"    // SendpOutboundRx/Tx + AudioCodecId for create_rnet wire-up
#include "wire/RbpFilter.h"  // prbpfilter / prbpfilter2 allocator targets

namespace lyra::wire {

// All members carry in-class default initializers, so the
// default ctor/dtor do zero work beyond letting C++ value-init
// each member.  Std::mutex members default-construct via RAII;
// `volatile long wb_enable` zero-inits via in-class `{0}`;
// arrays of sub-structs value-init via each sub-struct's own
// in-class defaults.
RadioNet::RadioNet()  = default;
RadioNet::~RadioNet() = default;

// §1.12 — Global instance pointer.  Owned + assigned by the
// wire-layer initializer at HL2 session start (Phase 2 wire-up,
// §10.3 step 14+); stays nullptr until then.  Name mirrors the
// reference's `RADIONET prn` (network.h:291) verbatim per Rule 1
// reference-parity grep discipline.
RadioNet* prn = nullptr;

// §1.12 supplement — `create_rnet()` allocator.  Direct verbatim
// mirror of the reference's `create_rnet()` at
// `netInterface.c:1590-1763`.  See RadioNet.h declaration for the
// full reference-vs-Lyra-idiom mapping rationale.  Sets the global
// `prn` (== reference's `prn`) on first call; idempotent on
// subsequent calls.
void create_rnet() {
    // No idempotency guard — reference at netInterface.c:1591-1596
    // unconditionally `prn = malloc(sizeof(radionet))`; second
    // call leaks.  Caller (HL2Stream::open) owns the call-once
    // discipline.  Operator-locked "do as reference, period"
    // (2026-06-06): no Lyra-native re-entry safety, no defensive
    // guard — match the reference posture.

    // `bandwidth_monitor_reset();` at netInterface.c:1597.
    // FIXME (Task #114): Lyra has no bandwidth-meter subsystem
    // yet; reference's `bandwidth_monitor_reset` zeros per-second
    // RX/TX byte counters polled by the operator UI.  Stub call
    // site preserved here (commented) so the eventual port lands
    // at the reference-faithful location.
    // bandwidth_monitor_reset();

    // `prn = (RADIONET)malloc(sizeof(radionet));`
    // (netInterface.c:1595)
    prn = new RadioNet();

    // Scalar field initialization, line-by-line mirror of
    // netInterface.c:1597-1620 (the post-malloc block under
    // `if (prn) { ... }`).  Lyra in-class defaults cover any
    // field the reference writes as 0; we explicitly write the
    // ones the reference writes to non-zero (or to a value that
    // doesn't match Lyra's in-class default) for full parity.

    prn->base_outbound_port  = 1024;       // :1598
    prn->p2_custom_port_base = 1025;       // :1599
    // :1610 prn->run = 0 — in-class default
    // :1611 prn->wdt = 0 — in-class default
    prn->sendHighPriority    = 1;          // :1612 (Lyra default 0)
    prn->num_adc             = 1;          // :1613 (Lyra default 0)
    prn->num_dac             = 1;          // :1614 (Lyra default 0)
    // :1615-1620 ptt_in/dot_in/dash_in/pll_locked/cc_seq_no/
    //   cc_seq_err = 0 — in-class defaults

    // i2c sub-struct (netInterface.c:1622-1630) — all zero,
    // covered by Lyra's in-class defaults.  Re-stated here only
    // if Lyra's in-class default ever drifts non-zero.

    // cw sub-struct (netInterface.c:1632-1639) — all zero EXCEPT
    // edge_length = 7 (the reference's only non-zero CW field at
    // create-time).
    prn->cw.edge_length      = 7;          // :1639 (Lyra default 0)

    // mic sub-struct (netInterface.c:1641-1643)
    // :1641 mic_control = 0 — in-class default
    // :1642 line_in_gain = 0 — in-class default
    prn->mic.spp             = 64;         // :1643 (Lyra default 0)

    // WB chain (netInterface.c:1646-1652)
    prn->wb_base_dispid          = 32;     // :1646 (Lyra default 0)
    // :1647 wb_enable = 0 — in-class default
    prn->wb_samples_per_packet   = 512;    // :1648
    prn->wb_sample_size          = 16;     // :1649
    prn->wb_update_rate          = 70;     // :1650
    prn->wb_packets_per_frame    = 32;     // :1651
    // :1652 lr_audio_swap = 0 — in-class default
    // :1653 CATPort = 0 — in-class default

    // ADC array (netInterface.c:1654-1664) — per-element init.
    // `tx_step_attn = 31` is the only non-default-zero scalar;
    // `wb_buff = malloc0(1024 * sizeof(double))` is allocated
    // per element (:1663).  HL2 doesn't currently consume
    // `wb_buff` (ANAN wide-band feature) but the reference
    // allocates eagerly for ALL adc[] entries at startup,
    // regardless of consumer — operator-locked "do as
    // reference, period" (2026-06-06) requires the same.
    for (int i = 0; i < kMaxAdc; i++) {
        prn->adc[i].id              = i;
        // rx_step_attn = 0 — in-class default
        prn->adc[i].tx_step_attn    = 31;  // :1657
        // adc_overload/dither/random/wb_seqnum/wb_state = 0 — defaults
        prn->adc[i].wb_buff.assign(1024, 0.0);  // :1663
    }

    // RX array (netInterface.c:1666-1692) — per-element init.
    for (int i = 0; i < kMaxRxStreams; i++) {
        prn->rx[i].id               = i;
        // rx_adc/frequency/enable/sync = 0 — in-class defaults
        prn->rx[i].sampling_rate    = 48;  // :1673
        prn->rx[i].bit_depth        = 24;  // :1674
        // preamp/rx_in_seq_no/etc. = 0 — defaults
        prn->rx[i].spp              = 238; // :1681
        // snapshots_head/tail/snapshot = NULL — Lyra-native
        //   snapshot machinery is not present in this layer
        //   (reference-specific P2 diagnostic) — flag as a
        //   §6-Q candidate if P2 work brings it in.
    }

    // TX array (netInterface.c:1694-1715) — per-element init.
    for (int i = 0; i < kMaxTxStreams; i++) {
        prn->tx[i].id               = i;
        // frequency = 0 — in-class default
        prn->tx[i].sampling_rate    = 192; // :1698
        // cwx/cwx_ptt/dash/dot/ptt_out/drive_level/phase_shift/
        //   epwm_max/epwm_min/pa = 0 — defaults
        prn->tx[i].tx_latency       = 20;  // :1709
        prn->tx[i].ptt_hang         = 12;  // :1710
        // mic_in_seq_no etc. = 0 — defaults
        prn->tx[i].spp              = 240; // :1714
    }

    // Audio array (netInterface.c:1717-1720)
    for (int i = 0; i < kMaxAudioStreams; i++) {
        prn->audio[i].spp           = 64;  // :1719
    }

    // Final scalars (netInterface.c:1722-1725) — all zero,
    // covered by Lyra's in-class defaults:
    // :1722 puresignal_run = 0
    // :1724 reset_on_disconnect = 0
    // :1725 swap_audio_channels = 0

    // `prbpfilter` + `prbpfilter2` allocation — reference-verbatim
    // mirror of netInterface.c:1727-1733:
    //
    //   prbpfilter = (RBPFILTER)malloc0(sizeof(rbpfilter));
    //   prbpfilter->bpfilter = 0;
    //   prbpfilter->enable = 1;
    //   prbpfilter2 = (RBPFILTER2)malloc0(sizeof(rbpfilter2));
    //   prbpfilter2->bpfilter = 0;
    //   prbpfilter2->enable = 2;
    //
    // FrameComposer's compose_case_0 / compose_case_10 /
    // compose_case_16 read these structs and assert non-null
    // — without create_rnet allocation, the composer asserts
    // trip on first invocation.  C↔C++23 idiom translation
    // malloc0 → new (zero-init via class default-ctor).  The
    // RbpFilter/RbpFilter2 class default-ctors set enable=1/2
    // via in-class defaults; the explicit assignments below
    // match the reference's explicit writes verbatim.

    // Reference sets thread handles to NULL at :1735-1740.  Lyra
    // uses `std::thread` + `std::counting_semaphore` with RAII
    // default-construct (zero-equivalent state).  No-op here.

    // Reference initializes Win32 CRITICAL_SECTIONs at :1742-1747.
    // Lyra uses `std::mutex` with RAII default-construct.  No-op.

    // ---- Buffer allocations (netInterface.c:1600-1608) ----
    //
    // All buffer sizing happens HERE in create_rnet, exactly
    // mirroring the reference.  Previously (§1-C-signed)
    // these were split across `outbound_init()` (outLRbufp +
    // outIQbufp), `Ep6RecvThread::start()` (RxBuff + TxReadBufp),
    // and `Ep2SendThread::start()` (OutBufp).  The 2026-06-06
    // operator audit caught the split as a §6-Q-class deviation
    // from the reference's single-allocation-site pattern; this
    // create_rnet body consolidates per the operator-locked
    // "do as reference, period" directive.  See PARITY_CHECKPOINTS
    // §14 Stage 1 (corrected).

    // `prn->RxBuff = calloc(8, sizeof(double*));`
    // `for (i = 0; i < 8; i++) prn->RxBuff[i] = calloc(64, 2*sizeof(double));`
    // (netInterface.c:1600-1602)
    prn->RxBuff.assign(8, std::vector<double>(64 * 2, 0.0));

    // `prn->RxReadBufp = calloc(1, 2*sizeof(double)*240);`
    // (netInterface.c:1603) — 480 doubles
    prn->RxReadBufp.assign(2 * 240, 0.0);

    // `prn->TxReadBufp = calloc(1, 2*sizeof(double)*720);`
    // (netInterface.c:1604) — 1440 doubles
    prn->TxReadBufp.assign(2 * 720, 0.0);

    // `prn->ReadBufp = calloc(1, sizeof(unsigned char)*1444);`
    // (netInterface.c:1605) — 1444 bytes
    prn->ReadBufp.assign(1444, 0);

    // `prn->OutBufp = calloc(1, sizeof(char)*1440);`
    // (netInterface.c:1606) — 1440 bytes
    prn->OutBufp.assign(1440, 0);

    // `prn->outLRbufp = calloc(1, sizeof(double)*1440);`
    // (netInterface.c:1607) — 1440 doubles
    prn->outLRbufp.assign(1440, 0.0);

    // `prn->outIQbufp = calloc(1, sizeof(double)*1440);`
    // (netInterface.c:1608) — 1440 doubles.  Note: reference at
    // networkproto1.c:1225 reads `outIQbufp + 256` in EER mode
    // (per `pcm->xmtr[0].peer->run && XmitBit`), which would
    // overrun a smaller allocation — the 1440-double size is
    // load-bearing for EER mode, even though current HL2 SSB
    // bring-up uses only the front 252 doubles.
    prn->outIQbufp.assign(1440, 0.0);

    // ---- prbpfilter / prbpfilter2 (netInterface.c:1727-1733) ----
    prbpfilter = new RbpFilter();
    prbpfilter->bpfilter = 0;  // :1728
    prbpfilter->enable   = 1;  // :1729
    prbpfilter2 = new RbpFilter2();
    prbpfilter2->bpfilter = 0;  // :1732
    prbpfilter2->enable   = 2;  // :1733

    // ---- cmaster outbound callback registration (Stage A, 2026-06-08) ----
    //
    // Reference netInterface.c:1749-1761:
    //
    //     switch (pcm->audioCodecId) {
    //     case HERMES:
    //         SendpOutboundRx(OutBound);
    //         break;
    //     case ASIO:
    //         SendpOutboundRx(asioOUT);
    //         break;
    //     case WASAPI:
    //         // not implmented
    //         break;
    //     }
    //     SendpOutboundTx(OutBound);
    //
    // Stage A scope: the setters STORE the callback in
    // `pcm->Outbound{Rx,Tx}`; the downstream wire-up (aamix's
    // `SetAAudioMixOutputPointer` for RX; ILV's `SetILVOutputPointer`
    // for TX) lands when Stage B (aamix port) + Stage C (ilv port)
    // ship.  Until then the stored callbacks sit idle — no behaviour
    // change from pre-Stage-A.  This is the architectural shell that
    // subsequent stages plug into; byte-shape matches reference
    // create_rnet's final block.
    //
    // The HERMES branch passes a thin wrapper around Lyra's existing
    // `outbound_push_lr` producer (declared in OutboundRing.h) — the
    // reference's `OutBound` function at network.c:1285-1340 is the
    // producer-side that does `memcpy → ReleaseSemaphore(hsendLRSem)
    // → WaitForSingleObject(hobbuffsRun)` for the RX audio path.
    // Lyra's `outbound_push_lr` is the byte-shape-equivalent of that
    // producer.  When Stage B's aamix lands, AAMIX will invoke this
    // callback for every RX audio frame it produces — matching
    // reference's `xMixAudio → AAMIX → OutboundRx → OutBound` chain.
    //
    // The ASIO branch is a Stage E+ FIXME (cmasio.c port pending).
    // The WASAPI branch matches reference's `// not implmented`
    // empty body.
    switch (pcm->audioCodecId) {
    case AudioCodecId::HERMES:
        SendpOutboundRx([](int id, int nsamples, double* buff) {
            (void) id;
            (void) nsamples;
            // Stage A NO-OP body: aamix doesn't exist yet, so this
            // callback is never invoked.  Stage B aamix port wires
            // it via SetAAudioMixOutputPointer; the eventual body
            // calls outbound_push_lr(buff) — the byte-shape-
            // equivalent of reference network.c:1335-1337
            // `memcpy(prn->outLRbufp,…); ReleaseSemaphore(hsendLR)
            // ; WaitForSingleObject(hobbuffsRun[1])`.
            (void) buff;
        });
        break;
    case AudioCodecId::ASIO:
        // FIXME (Stage E+ cmasio.c port): pass asio_out wrapper.
        // For now: matches reference WASAPI empty branch (no
        // registration; ASIO path doesn't drive the wire).
        break;
    case AudioCodecId::WASAPI:
        // not implmented — matches reference comment verbatim.
        break;
    }
    SendpOutboundTx([](int id, int nsamples, double* buff) {
        (void) id;
        (void) nsamples;
        // Stage A NO-OP body: ilv doesn't exist yet, so this
        // callback is never invoked.  Stage C ilv port wires it
        // via SetILVOutputPointer; eventual body calls
        // outbound_push_iq(buff) — byte-shape-equivalent of
        // reference's xilv → OutBound chain.
        (void) buff;
    });

    // ---- §15.26 PA-OFF-at-startup safety (Task #114, 2026-06-08) ----
    //
    // Initialize `ApolloFilt = 0x04` (C2 bit 2 = `tr_disable` per
    // ad9866.v gateware RTL).  This is the same state
    // `set_pa_on(false)` produces — consistent safe default.  PA
    // bias OFF (`ApolloTuner = 0` per BSS default) + T/R relay
    // disabled (`ApolloFilt = 0x04`) means **no RF can leave the
    // host on a MOX edge until the operator opts in to PA via
    // `set_pa_on(true)`** — neither driver-only leakage nor PA
    // output reach the antenna terminal.
    //
    // The other 4 Apollo globals (`ApolloTuner`, `ApolloFiltSelect`,
    // `ApolloATU`, `xvtr_enable`) stay at 0 (BSS default) per the
    // §15.26 PA-OFF-at-startup contract; comment block above the
    // global declarations at the bottom of this file documents the
    // safe-state pairing.
    ApolloFilt = 0x04;
}

// §3.4 — Dispatch-relevant runtime globals.
//
// `XmitBit` mirrors the reference verbatim (`network.h:413`).
// Reference type is plain `int` (Rule 24 source-verified
// 2026-06-05); zero at startup; written from the FSM's MOX-edge
// code in Phase 2 wire-up.  No atomic wrapper — reference posture
// preserved.
//
// `hpsdrModel` defaults to `HERMESLITE` (HL2 / HL2+ — the current
// Lyra target); replaced at session start by discovery + the
// per-family init when ANAN / Atlas / Saturn tester hardware
// arrives.  `radioProtocol` defaults to `USB` (Protocol 1 — HL2
// is P1).  Both variables are renamed from the reference's C-style
// enum-name shadow per §3.4 (acceptable deviation; role preserved).
int           XmitBit       = 0;
HPSDRModel    hpsdrModel    = HPSDRModel::HERMESLITE;
RadioProtocol radioProtocol = RadioProtocol::USB;

// §3.5 — Supplemental dispatch globals (`network.h:501-506`).
//
// `nddc` — per-family DDC count.  HL2 / HL2+ default is 4; per-
// family init at session start overwrites for non-HL2 families
// (Hermes II = 2; ANAN 7000DLE = 7; etc.).
//
// `SampleRateIn2Bits` — outbound sample-rate 2-bit code (48k=0,
// 96k=1, 192k=2, 384k=3).  Default 0 = 48k; operator rate setter
// writes per session.
//
// `P1_en_diversity` — diversity-enabled flag (0=off, non-zero=on);
// HL2 has no diversity feature, default 0 stays.
int           nddc              = 4;
unsigned char SampleRateIn2Bits = 0;
int           P1_en_diversity   = 0;

// §3.5 supplement (added 2026-06-05 per §4b-1 source-verification).
// `P1_adc_cntrl` — per-family ADC-to-DDC routing.  HL2 / HL2+ uses
// ADC0 for all DDCs, so default 0 works on the wire (case 4 emits
// C1=0, C2=0).  ANAN models set non-zero values at session open.
int P1_adc_cntrl = 0;

// §4b-2 supplement (added 2026-06-05 per §4b-2 source-verification;
// `ApolloFilt` default semantic corrected 2026-06-08 per Task #114
// PA-policy wire-layer fix).
//
// Globals default 0 (BSS) here at the file-scope declaration level;
// `create_rnet()` ABOVE writes `ApolloFilt = 0x04` to flip to the
// safe state (tr_disable set when PA off — same state
// `set_pa_on(false)` produces).  The BSS-0 default here covers the
// brief window between program start and `create_rnet()` invocation
// (no wire traffic is possible in that window since `prn == nullptr`).
//
// Post-`create_rnet()` safe state:
//   - `ApolloTuner = 0`     → C2 bit 3 clear → gateware `pa_enable = 0`
//                             → PA bias OFF
//   - `ApolloFilt  = 0x04`  → C2 bit 2 set   → gateware `tr_disable = 1`
//                             → T/R relay disabled even during MOX
//                             → no RF can leave the host until operator
//                               opts in via `set_pa_on(true)`
//   - `ApolloFiltSelect / ApolloATU / xvtr_enable = 0`
//                           → Apollo-daughterboard operator features
//                             OFF; operator opts in via dedicated
//                             setters (deferred until daughterboard
//                             support lands).
int xvtr_enable      = 0;
int ApolloFilt       = 0;   // overwritten to 0x04 by create_rnet (safe default)
int ApolloFiltSelect = 0;
int ApolloTuner      = 0;
int ApolloATU        = 0;

// §5 supplement (added 2026-06-05 per §5 Ep6RecvThread
// source-verification; default values corrected 2026-06-05 per
// §5-A audit to match reference BSS-init exactly).
//
// `mic_decimation_factor` default 0 mirrors the reference's
// `int mic_decimation_factor;` BSS-zero initialization
// (`network.h:507`).  With factor=0 the EP6 reader's
// `if (mic_decimation_count == mic_decimation_factor)` check
// never matches (count is post-incremented from 0 → 1 first
// iteration; never compares equal to 0), so NO mic harvest
// fires until a per-family setter writes a non-zero factor at
// session open.  HL2 default (every slot harvested) corresponds
// to factor=1, set by the equivalent of cmaster init in Phase 2
// wire-up.  Shipping factor=0 here preserves the reference's
// "silent until setter" posture verbatim.
//
// `mic_decimation_count` default 0 matches reference; also
// re-zeroed at EP6 reader thread entry per
// `networkproto1.c:424`.
int mic_decimation_factor = 0;
int mic_decimation_count  = 0;

}  // namespace lyra::wire
