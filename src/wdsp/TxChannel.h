// Lyra — TXA channel wrapper (§6 / §10.2 wdsp layer).
//
// Stage 2b2d — populated body per docs/TX_ARCHITECTURAL_MAPPING.md §6.
//
// Reference-faithful 1:1 wrapper around the WDSP TXA channel lifecycle.
// Each method invokes the reference WDSP function with byte-identical
// parameters; the C++23 class is the Rule 26 idiom translation of the
// reference's `pcm->xmtr[i]` struct + free-function pattern in
// `ChannelMaster/cmaster.c create_xmtr` / `destroy_xmtr`.
//
// What this class IS:
//   * RAII open/close of a WDSP TXA channel.
//   * `OpenChannel(channel, in_size, 4096, 48000, 96000, 48000,
//                  type=1, state=0, 0, 0.010, 0, 0.010, 1)` per
//     `cmaster.c:177-190`.  `in_size` and the output-buffer size
//     are BOTH derived from rates via the reference
//     `getbuffsize(rate) = 64 * rate / 48000` formula
//     (`cmsetup.c:106-111`) — never independently configured.
//   * Three TX output buffers allocated per `cmaster.c:126-127`
//     (`out[0]` = TX output, `out[1]` = EER output reserved,
//     `out[2]` = sidetone output reserved).  Reference allocates
//     all three unconditionally at channel-open; we match that
//     posture.  `out[1]`/`out[2]` are unused until EER + sidetone
//     helpers land in their own queued stages.
//   * `start()` -> `SetChannelState(ch, 1, 0)`.
//   * `stop()`  -> `SetChannelState(ch, 0, 1)` (always blocking
//     flush -- reference has no non-blocking TX-stop call site).
//   * `process(mic_iq, n_samples)` -> `fexchange0(ch, in, out, &err)`.
//   * `setMode(m)` -> `SetTXAMode`.
//   * `setBandpass(lo, hi)` -> `SetTXABandpassFreqs`.
//
// What this class is NOT (deferred to later queued stages per
// `docs/TX_ARCHITECTURAL_MAPPING.md §10.3`):
//   * `create_dexp` (VOX), `create_aamix` (anti-VOX),
//     `create_txgain` (Penelope / PS gain), `create_eer` /
//     `create_ilv` (EER), `create_sidetone` (CW sidetone),
//     `XCreateAnalyzer` (TX-side panadapter source).  Each ships
//     in its own stage with its own reference-verify cycle.
//
// LANDMINE — do NOT add `SetTXABandpassRun(ch, 1)`.
//
// The reference `create_xmtr` does NOT call `SetTXABandpassRun`
// anywhere (verified — string absent from the entire Thetis tree).
// `bp0` is set up by `create_bandpass(run=1, ...)` at `TXA.c:829`
// and reconfigured by `SetTXAMode` + `SetTXABandpassFreqs` (both
// of which call `TXASetupBPFilters` internally — that is the
// canonical mechanism).  `SetTXABandpassRun(ch, 1)` toggles
// `bp1.run` (the compressor-aux bandpass, NOT bp0); enabling it
// without the compressor configuring bp1 routes the stale
// create-time `(-5000,-100)` impulse after `bp0` and kills SSB.
// This was the §15.23 root cause that took multi-day debugging
// to find.  Keep it out.

#pragma once

#include <array>
#include <vector>

namespace lyra::dsp {
class WdspNative;  // forward decl — full def in `wdsp_native.h`
}

namespace lyra::wdsp {

// Reference buffer-size formula -- `cmsetup.c:106-111`:
//
//     int getbuffsize(int rate) {
//         const int base_rate = 48000;
//         const int base_size = 64;
//         return base_size * rate / base_rate;
//     }
//
// Yields constant latency across rates.  HL2 mic 48 kHz -> 64
// samples; 96 k -> 128; 192 k -> 256; 384 k -> 512.  Linear, NO
// power-of-2 rounding.
constexpr int referenceBuffsize(int rate) noexcept
{
    return 64 * rate / 48000;
}

// TXA mode enum.  Order + integer values match the reference
// `wdsp/TXA.h:31-47 enum txaMode` 1:1 (Lyra-native naming, same
// underlying values feed `SetTXAMode`).
enum class TxaMode : int {
    LSB    = 0,
    USB    = 1,
    DSB    = 2,
    CWL    = 3,
    CWU    = 4,
    FM     = 5,
    AM     = 6,
    DIGU   = 7,
    SPEC   = 8,
    DIGL   = 9,
    SAM    = 10,
    DRM    = 11,
    AM_LSB = 12,
    AM_USB = 13,
};

// Channel-open configuration.  Defaults match the reference
// `cmaster.c:177-190` `OpenChannel` call for HL2:
//   inRate   -- 48000 Hz (HL2 AK4951 mic; CLAUDE.md §3.5)
//   dspSize  -- 4096 per reference (hardcoded TXA internal)
//   dspRate  -- 96000 Hz per reference
//   outRate  -- 48000 Hz (HL2 EP2 TX IQ; CLAUDE.md §3.5)
//   tslewup / tslewdown -- 10 ms cos² ramp per reference
//   block    -- 1 (block-until-output-available)
//
// Note `inSize` is NOT a field -- the reference derives it from
// `inRate` via `getbuffsize(inRate)` (cmsetup.c:71).  Same for
// the output buffer size which uses `getbuffsize(outRate)` per
// cmsetup.c:84.  Both are computed at `open()` time.
struct TxConfig {
    int    inRate     = 48000;
    int    dspSize    = 4096;
    int    dspRate    = 96000;
    int    outRate    = 48000;
    double tDelayUp   = 0.000;
    double tSlewUp    = 0.010;
    double tDelayDown = 0.000;
    double tSlewDown  = 0.010;
    int    block      = 1;
};

class TxChannel {
public:
    // RAII-non-owning ctor.  `wdsp` must outlive the TxChannel.
    // `channel_id` is the WDSP channel slot (TX uses 1 by
    // convention; RX1 occupies 0 via `WdspEngine`).
    TxChannel(lyra::dsp::WdspNative& wdsp, int channel_id,
              TxConfig cfg = {});
    ~TxChannel();

    TxChannel(const TxChannel&)            = delete;
    TxChannel& operator=(const TxChannel&) = delete;

    // OpenChannel + buffer allocation.  Returns false if the WDSP
    // DLL isn't loaded or required symbols aren't resolved.
    // Idempotent — second call is a no-op.
    // Reference: `cmaster.c:177-190` (OpenChannel) + 126-127
    // (xmtr[i].out[0..2] allocation).
    bool open();

    // CloseChannel alone.  Reference `destroy_xmtr`
    // (cmaster.c:255-271) calls
    // `CloseChannel (chid (inid (1, i), 0))` with NO
    // preceding SetChannelState at line 265 -- the
    // blocking-flush SetChannelState(0,1) discipline
    // belongs at keyup (`stop()`; reference
    // console.cs:30355), NOT at destroy time.  Strict
    // reference fidelity per operator-locked methodology.
    // Idempotent.
    void close();

    // Start the channel running.  `SetChannelState(ch, 1, 0)`.
    // Reference: `console.cs:30346` keydown final step.
    // Channel must be open()'d first; no-op if already running.
    void start();

    // Stop the channel.  `SetChannelState(ch, 0, 1)` — ALWAYS
    // blocking-flush.  Reference (`console.cs:30355` keyup +
    // `destroy_xmtr` cmaster.c:265): the only TX-channel-stop
    // sites both use dmode=1; there is no non-blocking
    // TX-stop call site anywhere in the reference.  No-op if
    // not running.
    void stop();

    // Operator-band setters — exposed but NOT called at open()
    // time (reference posture: `create_xmtr` does NOT call
    // SetTXAMode / SetTXABandpassFreqs; those are runtime
    // setters from console.cs at operator mode/filter changes).
    // Stage 2b2e+ wires these to the operator UI.
    void setMode(TxaMode m);
    void setBandpass(double lo_hz, double hi_hz);

    // Mic input -> TXA chain -> TX I/Q output.  `mic_iq` is
    // interleaved {I=mic_double, Q=0.0} doubles per the Stage
    // 2b2c `Hl2Ep6MicSource::Consumer` contract (which is itself
    // byte-shape-identical to reference's
    // `Inbound(inid(1,0), n, prn->TxReadBufp)` --
    // `networkproto1.c:404-407` / `:570-573` / `cmbuffs.c:89`).
    // `n_samples` is the pair count; the buffer length is
    // `2 * n_samples` doubles.
    //
    // Calls `fexchange0(channel_, in_buf, out_buf, &err)`.
    // Returns the WDSP error code from `err` (0 = success).
    //
    // `const_cast` at the wrapper boundary is the standard
    // C-to-C++23 const-correctness bridge -- WDSP's
    // `fexchange0` cdef takes `double*` because C doesn't
    // annotate const consistently, but it only READS through
    // the input pointer (Rule 26 idiom translation; not a
    // semantic deviation).
    //
    // The `!opened_` early-return is a C++23 RAII state guard
    // (Rule 26 idiom) -- the reference assumes the caller has
    // set the channel up; we make that explicit at the API
    // boundary.  When the channel IS open, behaviour is
    // byte-identical to a bare fexchange0 call.
    //
    // Wire-inert at Stage 2b2d: nothing constructs this class
    // and nothing calls process(); the output is generated but
    // not consumed.  Stage 2b2e wires it via
    // `Hl2Ep6MicSource::setConsumer(...)`.
    int process(const double* mic_iq, int n_samples);

    // Pre-allocated TX output buffer, interleaved {I,Q} doubles,
    // sized to `referenceBuffsize(outRate)` pairs (= 64 pairs
    // for HL2 at 48 kHz).  Stable pointer for the channel's
    // lifetime.  Content overwritten per `process()` call.
    // Reference: `xmtr[i].out[0]` (cmaster.c:127).
    const double* outIq() const noexcept { return outBuf_.data(); }
    int           outSize() const noexcept { return outSize_; }

    // Three-buffer view for the Stage D xilv() dispatch.  Matches
    // reference's `pcm->xmtr[i].out[3]` layout byte-for-byte:
    //   [0] = TX I/Q output       (post-TXA)        -- outBuf_
    //   [1] = EER output          (reserved/zeros)  -- out1Buf_
    //   [2] = sidetone output     (reserved/zeros)  -- out2Buf_
    //
    // Used at the xcmaster TX-case-1 site (cmaster.c:407
    // `xilv(pcm->xmtr[tx].pilv, pcm->xmtr[tx].out)`); Lyra's
    // equivalent passes the result of this method as the `double**`
    // argument.  Returns non-const pointers because xilv writes into
    // its own outbuff but READS from data[i] -- the WDSP API has C
    // const-discipline gaps in the read path (same Rule 26 idiom
    // translation TxChannel::process() applies to the `mic_iq`
    // fexchange0 parameter; not a semantic deviation).
    //
    // Stable for the channel's lifetime.  Content of out[0] is
    // overwritten per process() call; out[1] / out[2] stay zero
    // until EER / sidetone helpers land in their own queued stages.
    std::array<double*, 3> outBuffers() noexcept {
        return { outBuf_.data(), out1Buf_.data(), out2Buf_.data() };
    }

    // Reference-derived input block size: `getbuffsize(inRate)`.
    int inSize() const noexcept { return inSize_; }

    int  channelId() const noexcept { return channel_; }
    bool isOpen()    const noexcept { return opened_; }
    bool isRunning() const noexcept { return running_; }

private:
    lyra::dsp::WdspNative& wdsp_;
    int         channel_;
    TxConfig    cfg_;
    bool        opened_  = false;
    bool        running_ = false;

    // Derived from rates via referenceBuffsize() at open() time.
    int inSize_  = 0;
    int outSize_ = 0;

    // Reference cmaster.c:126-127 allocates THREE TX output
    // buffers per xmtr (TX / EER / sidetone).  All three are
    // allocated unconditionally at channel-open even when EER
    // is off and CW isn't active.  Stage 2b2d matches that
    // posture; `out1Buf_` and `out2Buf_` sit unused until EER
    // and sidetone helpers land in their own queued stages.
    std::vector<double> outBuf_;     // out[0] -- TX output
    std::vector<double> out1Buf_;    // out[1] -- EER output (reserved)
    std::vector<double> out2Buf_;    // out[2] -- sidetone output (reserved)
};

}  // namespace lyra::wdsp
