// Lyra-cpp — CMaster.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/cmaster.h: cmaster typedef + pcm global +
//                              xcmaster + SendpOutbound* signatures
//   - ChannelMaster/cmaster.c: create_cmaster / destroy_cmaster /
//                              xcmaster body / SendpOutbound* setters
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014-2019 Warren Pratt, NR0V
//                     and the openHPSDR / Thetis contributors
// License: GNU General Public License v3 or later
// Lyra-cpp is also GPL v3+; redistribution complies with GPL
// terms (preserved copyright, documented modifications, complete
// source available at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// **C → C++23 idiom translations applied** (each preserves
// observable behaviour; see per-symbol comments for exceptions):
//   - struct typedef         → struct with in-class initializers
//   - C unscoped enum        → enum class (with explicit int values)
//   - C function pointers    → std::function (per docs/RULES.md §5.8
//                              signed-off idiom)
//   - CRITICAL_SECTION       → std::mutex (per Q5 Option A)
//   - _InterlockedExchange   → plain int assignment for x86_64
//                              word-sized writes (Rule 24 source-
//                              verified; matches XmitBit precedent
//                              at RadioNet.h:716-727)
//
// [Lyra-native] markers identify additions that are not part of
// the reference port — Lyra TX DSP enhancements, operator-policy
// hooks, etc.
//
// See NOTICE.md and CREDITS.md (repo root) for full attribution.
//
// ---------------------------------------------------------------
//
// **Multi-stage port** (operator-directed 2026-06-08, "FIX IT LIKE
// THETIS DOES — no deviations no patches"):
//
//   Stage A (THIS COMMIT): module shell.  cmaster struct typedef +
//     global pcm pointer + enum AudioCodecId + 4 SendpOutbound* setter
//     stubs + create_rnet `switch (audioCodecId)` wire-up per
//     netInterface.c:1749-1761.  NO behaviour change yet — the setters
//     store the callback pointers but no xcmaster pump invokes them
//     (the pump body lands in Stage B+).  The 4 callback storage slots
//     are the architectural place subsequent stages plug into.
//
//   Stage B (PENDING): port `aamix.c` / `aamix.h`.  Replaces Lyra's
//     existing audio_mixer with reference-faithful AAMIX.  Wires
//     `SendpOutboundRx` into `SetAAudioMixOutputPointer(0, 0, cb)` so
//     aamix invokes the registered callback on every RX audio frame.
//
//   Stage C (PENDING): port `ilv.c` / `ilv.h`.  Wires `SendpOutboundTx`
//     into `SetILVOutputPointer(0, cb)` so ILV invokes the registered
//     callback on every TX I/Q frame.
//
//   Stage D (PARTIAL — D.0 THIS COMMIT): port `xcmaster()` pump body.
//     For HL2 SSB v0.2 the case-1 TX body reduces (per Stage E.0
//     audit findings) to:  fexchange0 → xilv.  All other reference
//     stages (xdexp / xsidetone / monitor xMixAudio / xtxgain / xeer)
//     are reference-`run=0` for HL2 OR deferred-by-design features
//     (CW sidetone v0.2.2, VOX v0.2.3, Penelope/PS v0.3) — same
//     posture the reference's create_xmtr takes.  Lyra-cpp ports the
//     full `xcmaster(int stream)` switch signature for reference-
//     shape fidelity; case 0 (RX) is a documented stub (Lyra dispatches
//     RX via WdspEngine, pre-existing architecture); case 2 (special
//     panadapter stitching) is not used.
//
//     Stage D sub-commits:
//       D.0 (this commit) — skeleton: pxmtr[] central bank + 4 decls
//                           (create_xmtr_hl2 / destroy_xmtr_hl2 /
//                            xcmaster / xcmasterTickTx).  Wire-inert.
//       D.1 — TxChannel::outBuffers() API extension (returns the
//             3 internal buffer ptrs as the reference pcm->xmtr[i]
//             .out[3] equivalent, needed for the xilv() double**
//             argument).
//       D.2 — Function bodies in CMaster.cpp.
//       D.3 — scratch/test_xcmaster.cpp synthetic-mic Outbound
//             capture test, asserting bit-exact TX I/Q dispatch
//             (ILV bypass mode per cmaster.c:227 run=0).
//
//     Stage D ships the xcmaster module wire-inert (no production
//     caller yet — the rebuild's consumer-side, currently in_progress
//     per Task #112/#117, wires it).  The unit test is the bench
//     gate for Stage D itself; the first-RF HL2 bench gate lands
//     with whichever later commit wires the rebuild's consumer
//     thread to call into xcmasterTickTx.
//
//   Stage E.0 (DONE) — TxChannel reference-parity audit (zero must-fix
//     except the surface #5 TX analyzer port, deferred to Stage E.1
//     post-Stage-D per PureSignal v0.3 prerequisite).  See
//     docs/architecture/STAGE_E_TXCHANNEL_AUDIT.md.
//
//   Stage E.1 (PENDING — post-D) — TX analyzer port to reference
//     parity (XCreateAnalyzer at TX-channel-open, retarget
//     TXASetSipDisplay, stop MOX-edge SetAnalyzer reconfigure).
//     PS prereq.  Task #140.
//
//   Stage E+ (PENDING) — port supporting modules (vox/dexp, txgain,
//     sidetone, pipe, sync, cmbuffs, cmsetup, bandwidth_monitor,
//     etc.) as each becomes needed by the rebuild + feature work.
//
// Each stage ships independently with a build-green + push step.
// HL2 bench gate per stage (RX audio path stays working until the
// migration to cmaster pump in Stage D is bench-verified).
//
// **Stage A scope (this commit) is intentionally NO-OP** — the shell
// is added, the create_rnet body matches reference :1749-1761
// byte-shape, but no caller invokes the registered callbacks yet
// because the cmaster pump doesn't exist yet.  This is the smallest
// honest first stage of a multi-stage port; the architectural shape
// is now byte-shape-matched to reference at the create_rnet body
// level, and subsequent stages fill in the behaviour.

#pragma once

#include <array>
#include <cstdint>
#include <functional>

// Forward decl — TxChannel full def in src/wdsp/TxChannel.h.  Kept
// out of this header's #includes to avoid pulling the WDSP cffi
// surface into every wire-layer translation unit.
namespace lyra::wdsp { class TxChannel; }

namespace lyra::wire {

// ===== Audio codec selector =====
//
// Reference `enum AudioCODEC` at cmaster.h:116-121.  Selects whether
// the radio's onboard codec (HERMES — the HL2/HL2+ AK4951 path) or
// the host's audio device (ASIO / WASAPI) drives RX audio output.
// `audio_codec_id` global mirrors `pcm->audioCodecId` at cmaster.h:70.
//
// Default `HERMES` matches HL2/HL2+ operating point (onboard codec
// drives RX audio via EP2).  ASIO support requires the `cmasio.c`
// port; WASAPI is "not implemented" in the reference as well.
//
// Reference uses `int` for the field type and unscoped enum values.
// Lyra uses `enum class` with explicit underlying type `int` for
// C++23-idiomatic safety; the integer values match reference exactly.
enum class AudioCodecId : int {
    HERMES = 0,   // HL2/HL2+ onboard codec via EP2 — Lyra default
    ASIO   = 1,   // host ASIO sound device (cmasio.c port pending)
    WASAPI = 2,   // host WASAPI — reference comments "not implmented"
};

// ===== Outbound / Inbound callback signatures =====
//
// Reference function pointer signatures at cmaster.h:65-68.
//
// Reference C signatures (kept here as comment for grep-parity):
//   void (*OutboundRx)(int id, int nsamples, double* buff)
//   void (*OutboundTx)(int id, int nsamples, double* buff)
//   void (*OutboundTCIRxIQ)(int id, int nsamples, double* buff)
//   void (*InboundTCITxAudio)(int nsamples, double* buff)
//
// Lyra uses `std::function` per the §5.8 signed-off C-function-pointer
// → C++23 `std::function` translation (same idiom as Router/xrouter).
using OutboundCallback = std::function<void(int id, int nsamples, double* buff)>;
using InboundCallback  = std::function<void(int nsamples, double* buff)>;

// ===== Global cmaster state =====
//
// Reference `cmaster cm = {0}; CMASTER pcm = &cm;` at cmaster.c:29-30.
// Process-lifetime; never freed (matches reference posture — `pcm`
// lives for the lifetime of the radio driver process).
//
// Stage A scope: only the dispatch-relevant fields are populated.
// The full cmaster struct (per-RCVR audio buffers, ANB/NOB noise
// blankers, per-XMTR DEXP/AAMIX/TXgain/EER/ILV/sidetone, etc.) lands
// in subsequent stages as the supporting modules port.
struct CMasterState {
    // Reference cmaster.h:70 — `int audioCodecId;`
    AudioCodecId audioCodecId = AudioCodecId::HERMES;

    // Reference cmaster.h:65-68 — the 4 callback function pointers.
    // Stored by SendpOutbound* setters; subsequent stages wire them
    // into aamix (RX) and ILV (TX) so the callbacks fire on every
    // frame the cmaster pump processes.  Until Stage B+ wires them
    // in, the stored callbacks sit idle (Stage A is no-op by design).
    OutboundCallback OutboundRx;
    OutboundCallback OutboundTx;
    OutboundCallback OutboundTCIRxIQ;
    InboundCallback  InboundTCITxAudio;

    // Reference cmaster.h:69 — `volatile long tci_run;`
    // Set via SetTCIRun().  Read at TCI dispatch sites in Stage D+
    // when xcmaster pump body lands.
    int tci_run = 0;
};

extern CMasterState  cm;
extern CMasterState* pcm;

// ===== SendpOutbound* / SendpInbound* setters =====
//
// Reference at cmaster.c:407-432.  Each setter stores the callback
// into `pcm->Outbound*` / `pcm->Inbound*` AND in the reference's
// HERMES / TX path additionally wires the callback into the
// downstream module (AAMIX for RX, ILV for TX).
//
// Stage A: setters store the callback into pcm->* fields only.
// The downstream module wire-up (SetAAudioMixOutputPointer in
// SendpOutboundRx; SetILVOutputPointer in SendpOutboundTx) lands
// when Stage B (aamix port) and Stage C (ilv port) ship.
//
// Reference C signatures preserved verbatim for grep-parity:
//   void SendpOutboundRx (void (*Outbound)(int id, int nsamples, double* buff))
//   void SendpOutboundTx (void (*Outbound)(int id, int nsamples, double* buff))
//   void SendpOutboundTCIRxIQ (void (*Outbound)(int id, int nsamples, double* buff))
//   void SendpInboundTCITxAudio (void (*Inbound)(int nsamples, double* buff))
void SendpOutboundRx(OutboundCallback cb);
void SendpOutboundTx(OutboundCallback cb);
void SendpOutboundTCIRxIQ(OutboundCallback cb);
void SendpInboundTCITxAudio(InboundCallback cb);

// ===== SetTCIRun (reference cmaster.c:434-437) =====
void SetTCIRun(int active);

// ===== Stage D — xcmaster pump + per-xmtr context bank =====
//
// Reference cmaster.c reaches the per-xmtr context (TxChannel-
// equivalent + pilv-equivalent) via the central `pcm->xmtr[tx]`
// struct — populated by `create_xmtr()` (cmaster.c:112-253) and
// torn down by `destroy_xmtr()` (cmaster.c:255-271).  Lyra-cpp
// doesn't have the full pcm->xmtr struct (Stage E would build it
// out — see Stage E.0 audit findings), so this central bank serves
// the same routing role: Stage D's `xcmaster(int stream)` pump
// resolves stream → xmtr_id → XmtrSlot and dispatches to the
// configured TxChannel + pilv slot.
//
// Same Lyra-native sidestep AAMix's paamix[] (Stage B) and ILV's
// pilv[] (Stage C) use — preserves reference call-site portability
// without painting Stage E's full reconciliation into a corner.

// Process-wide bank size.  v0.2 = single transmitter; bumped when
// RX2-era split-transmit lands.
inline constexpr int MAX_EXT_XMTR = 1;

// Per-xmtr slot: ties together the WDSP TXA channel wrapper +
// the ILV bank slot id.  Both pointers/ids are non-owning views;
// lifetime is managed by whoever calls create_xmtr_hl2.
struct XmtrSlot {
    // The TXA channel wrapper.  Stage D pump body calls
    // tx_channel->process(mic_in, n) which runs fexchange0 + fills
    // its out[0..2] buffers.  nullptr slot = no transmitter
    // configured (xcmaster pump tick is a safe no-op).
    lyra::wdsp::TxChannel* tx_channel = nullptr;

    // The ILV bank slot id for this xmtr (passed to xilv() lookup).
    // Matches reference convention: ILV xmtr_id == TX xmtr_id == 0
    // for the single-transmitter case.
    int ilv_xmtr_id = 0;
};

// [Lyra-native] Process-wide xmtr bank.  Populated by
// create_xmtr_hl2 (xmtr_id < MAX_EXT_XMTR), cleared by
// destroy_xmtr_hl2.  Used by xcmaster's TX-case dispatch to
// resolve stream → per-xmtr context.  Definition in CMaster.cpp.
extern std::array<XmtrSlot, MAX_EXT_XMTR> pxmtr;

// ===== create_xmtr_hl2 / destroy_xmtr_hl2 =====
//
// Lyra-cpp HL2 SSB minimum equivalent of reference
// cmaster.c:112-253 `create_xmtr()` / 255-271 `destroy_xmtr()`.
// The reference allocates 9 per-xmtr surfaces (out[0..2] / DEXP /
// anti-VOX AAMix / OpenChannel / Analyzer / TXGain / EER / ILV /
// sidetone); per Stage E.0 audit, 4 of those (out / OpenChannel /
// ILV) ship and 5 (DEXP / anti-VOX / Analyzer / TXGain / EER /
// sidetone) are reference-`run=0` for HL2 or deferred-by-design
// (CW v0.2.2, VOX v0.2.3, PS v0.3, TX analyzer Stage E.1).
//
// Lyra's create_xmtr_hl2 takes pre-constructed TxChannel + an
// already-created ILV bank slot id (via lyra::wdsp::create_ilv) —
// the wiring code outside CMaster is responsible for the lifecycle
// of those parts.  This commit only populates the central pxmtr[]
// bank so xcmaster can find them.  Returns nothing; out-of-range
// xmtr_id is a silent no-op.
void create_xmtr_hl2(int xmtr_id,
                     lyra::wdsp::TxChannel* tx_channel,
                     int ilv_xmtr_id);

// Clears the central pxmtr[] bank slot for xmtr_id (defensive
// against the create-then-destroy race: only clears if the slot
// still references tx_channel).  Does NOT destroy the TxChannel
// or ILV — caller owns those lifetimes.
void destroy_xmtr_hl2(int xmtr_id, lyra::wdsp::TxChannel* tx_channel);

// ===== xcmaster (reference cmaster.c:340) =====
//
// Reference signature: `PORT void xcmaster(int stream)`.  Switches
// on stype(stream): case 0 = RX, case 1 = TX, case 2 = special
// upper-panadapter stitch.  Lyra-cpp implements case 1 for HL2 SSB
// v0.2; case 0 is a documented stub (RX is dispatched via
// WdspEngine in Lyra-cpp's pre-existing architecture); case 2 is a
// documented stub (special-panadapter stitch is not used).
//
// Reference shape ported faithfully even though only case 1 has a
// body — future stages (RX migration into the central pump if/when
// the architecture warrants) plug into the existing switch without
// signature churn.
void xcmaster(int stream);

// ===== xcmasterTickTx (Lyra-native pump entry) =====
//
// Lyra-cpp helper that the xcmaster TX case-1 dispatches to.  In
// the reference, the case-1 body is inline; Lyra factors it into
// a named entry so consumer threads (the rebuild's TX pump) can
// call it directly with an explicit mic-input buffer + sample
// count — without needing the reference's pcm->in[stream] central
// buffer indirection.
//
// `xmtr_id` resolves through pxmtr[]; `mic_in` is interleaved
// {I=mic, Q=0} doubles per reference TxReadBufp convention
// (networkproto1.c:404-407 / :570-573); `n_samples` is the
// complex-tuple count (= reference's pcm->xcm_insize[stream] / 2).
//
// Pump body for HL2 SSB v0.2: TxChannel->process(mic_in, n) →
// xilv(pilv[ilv_xmtr_id], tx_channel->outBuffers().data()).
// The xilv call dispatches to ILV.Outbound which is the
// operator-registered SendpOutboundTx callback (the EP2 wire side).
void xcmasterTickTx(int xmtr_id, double* mic_in, int n_samples);

}  // namespace lyra::wire
