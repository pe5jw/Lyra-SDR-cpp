// Lyra — ChannelMaster (cmaster) layer.
//
// Reference-faithful port of the reference `cmaster.h` + `cmaster.c`
// orchestration layer.  Reference files (provenance only; the
// reference name stays out of shipped commits per the no-attribution
// rule):
//   - cmaster.h: cmaster typedef + pcm global + xcmaster + SendpOutbound*
//   - cmaster.c: create_cmaster / destroy_cmaster / xcmaster body /
//                SendpOutbound* setters
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
//   Stage D (PENDING): port `xcmaster()` pump body — the per-stream
//     RX-case (xpipe → xanb → xnob → Spectrum0 → analyzers →
//     fexchange0 → xpipe → xMixAudio) + TX-case (asioIN → xpipe →
//     xdexp → fexchange0 → xsidetone → xpipe → xMixAudio (monitor) →
//     xtxgain → xeer → xilv).  Existing audio_mixer + TxDspWorker
//     migrate from direct outbound_push_lr/iq invocation to driving
//     the xcmaster pump.
//
//   Stage E+ (PENDING): port supporting modules (vox/dexp, txgain,
//     sidetone, pipe, sync, analyzers, cmbuffs, cmsetup,
//     bandwidth_monitor, etc.) as each becomes needed.
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

#include <cstdint>
#include <functional>

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

}  // namespace lyra::wire
