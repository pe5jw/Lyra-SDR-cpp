// Lyra-cpp — TX-1 component 2: WDSP TXA channel lifecycle.
//
// Wraps one WDSP TXA channel for SSB voice TX.  Owns the channel's
// open/close + start/stop, the operator-facing setter surface
// (mode, bandpass, mic gain, ALC ceiling, leveler on/off, PHROT
// on/off), and the per-block fexchange0 hot-path call (the body
// of process() lands with the TX DSP worker, component 4 — this
// component declares the symbol and pins the lifecycle).
//
// LOAD-BEARING discipline (design v2 §4 + §5.3 + §15.23 lesson):
//
//   * Channel id 1 (RX1 is 0).  type=1 selects TXA.
//   * OpenChannel param block: in_size = 126 (per-datagram mic-
//     sample count on this radio family at nddc=4), dsp_size =
//     2048, in_rate = 48000, dsp_rate = 96000, out_rate = 48000,
//     state = 0 at open (started explicitly below), block = 1.
//   * In open(), push SetTXABandpassFreqs BEFORE SetTXAMode so
//     bp0 never runs on the create-time (-5000,-100) defaults
//     during the ~2.6 ms window TXASetupBPFilters fires off the
//     mode switch (wdsp/TXA.c:786).
//   * DELIBERATELY OMITTED: SetTXABandpassRun (toggles stale bp1
//     into the SSB path = wrong-sideband / no-output, §15.23
//     trap), SetTXAPanelSelect (create-time inselect=2 already
//     routes I=mic + Q=0 via patchpanel case-0), SetTXAALCThresh
//     (does not exist — MaxGain alone governs the ALC ceiling).
//   * ALC always on (mandatory splatter protection; no operator
//     opt-out; MaxGain operator-tunable later via Settings → TX).
//   * Leveler wired but OFF at open (operator opt-in via
//     Settings → TX).
//   * PHROT on at open (the WDSP create-time default is OFF; we
//     enable it for the PEP-to-PAR win on SSB voice).
//   * Per-mode sideband sign: operator passes positive low/high
//     (e.g. 200, 3100); the engine signs internally before the
//     setter call (USB: +low,+high; LSB: -high,-low; others
//     unused in TX-1).  See pushBandpassLocked() + signedEdges().
//
// Threading: not thread-safe by itself.  Component 4 (the TX DSP
// worker) owns the instance + does the locking; mainline / UI
// drives the operator setters from the main thread between
// process() invocations.

#pragma once

#include "wdsp_native.h"

#include <QString>

#include <atomic>
#include <complex>
#include <mutex>
#include <utility>
#include <vector>

namespace lyra::dsp {

class TxChannel {
public:
    enum class Mode { USB, LSB };   // TX-1 ships USB + LSB; other
                                    // modes land in later slices.

    // channelId is the WDSP channel number to open (TX = 1).
    // wdsp is the resolved DLL surface (nullptr-safe — every
    // method early-returns if any setter pointer is missing).
    TxChannel(int channelId, WdspNative *wdsp);
    ~TxChannel();

    TxChannel(const TxChannel &)            = delete;
    TxChannel &operator=(const TxChannel &) = delete;

    // Open the channel + push the locked init setter sequence.
    // Idempotent.  micRate / dspRate / outRate are forwarded
    // straight to OpenChannel; the design v2 §5.3 init pins
    // these at 48000 / 96000 / 48000 — values outside that band
    // would need a setter audit, so we log + bail if asked for
    // anything else.  Returns true on success.
    bool open(int micRate = 48000, int dspRate = 96000, int outRate = 48000);

    // Stop (blocking flush, dmode=1) + CloseChannel.  Idempotent.
    // Called automatically from the destructor.
    void close();

    // Channel state transitions.  start() = SetChannelState(1, 0)
    // (the wakeup keying signal — uslew envelope gated to
    // TUN / tone-gen only, NOT SSB mic per design v2 §4.9).
    // stop() = SetChannelState(0, 1) blocking flush (waits up to
    // 100 ms on WDSP's downslew before returning).
    void start();
    void stop();

    bool isOpen()    const { return opened_; }
    // Atomic load — safe to call from the TxDspWorker thread without
    // holding channelMtx_.  Task #46 fix (2026-05-31): the worker
    // skips fexchange0 when this returns false so RX-only quiescent
    // state isn't mis-classified as fexchange0 errors.  A racy read
    // around a start/stop edge at worst costs one extra fexchange0
    // returning -1 (channel state=0 in WDSP), or skips one block on
    // a fresh-start — both transient and benign.
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Operator-positive passband Hz; the engine signs internally
    // per the current mode.  Re-pushed atomically with the mode
    // (both call pushBandpassLocked() under the same code path).
    void setMode(Mode m);
    void setBandpass(double opLowHz, double opHighHz);

    // SetTXAPanelGain1 — operator mic gain.  WDSP takes a linear
    // gain; we accept dB at the public boundary for UI sanity.
    void setMicGainDb(double db);

    // ALC ceiling.  WDSP `SetTXAALCMaxGain` takes a LINEAR factor
    // (NOT dB) — pass-through with no unit conversion.  Verified-
    // reference UI exposes this as an integer spinner 0..120 with
    // a default of 3 (= 3.0× amplitude = +9.54 dB amplification
    // headroom before the ALC limiter pulls down).  Operator-
    // tunable; ALC run state is pinned ON in open() and not
    // exposed here (always-on splatter protection).
    void setAlcMaxGainLinear(double linear);

    // ALC decay time constant in ms.  WDSP `SetTXAALCDecay`
    // sets the exponential-curve tau the wcpagc envelope detector
    // uses to release.  Verified-reference UI: integer spinner
    // 1..50 ms incr 1, default 10 ms.  ALC attack stays at the
    // WDSP create-time default (1 ms) — no operator surface for
    // it (the reference UI doesn't expose attack either).
    void setAlcDecayMs(int decay_ms);

    // Leveler run + ceiling.  topLinear is the operator's max-
    // gain headroom as a LINEAR amplitude factor (NOT dB).
    // Verified-reference UI: integer spinner 0..20 LINEAR incr 1
    // default 15 (= 15× amplification = +23.5 dB pre-ALC boost
    // for weak signals).  WDSP API takes LINEAR direct (same
    // unit-mismatch class the ALC ceiling had; see §15.27).
    // Wired but defaults OFF in open() (operator opt-in).
    void setLevelerOn(bool on, double topLinear = 15.0);

    // Leveler decay time constant in ms.  WDSP `SetTXALevelerDecay`
    // sets the exponential-curve tau the wcpagc envelope detector
    // uses to release.  Verified-reference UI: integer spinner
    // 1..5000 ms incr 1, default 100 ms.  Leveler attack stays
    // at the WDSP create-time default (1 ms).
    void setLevelerDecayMs(int decay_ms);

    // PHROT (phase rotator).  ON at open() per design v2 §4.6
    // for the PEP-to-PAR win; operator may toggle from Settings.
    void setPhrotOn(bool on);

    // Hot path (component 4 fills the body).  mic_block carries n
    // mono samples at micRate; iq_out receives n complex<float>
    // TX-baseband samples.  Returns the WDSP fexchange0 error
    // code (0 = OK).  Returns -1 when the channel isn't open or
    // running, or while this lifecycle slice is shipping wire-
    // inert (no fexchange0 wiring yet) — that's why the body is
    // here as a stub: component 4's job is to wire mic → I-slot,
    // Q = 0, call fexchange0, unpack to complex<float>.
    int process(const float *mic_block, int n,
                std::complex<float> *iq_out);

    // Read-only introspection for diagnostics + bench scripts.
    Mode   mode()       const { return mode_; }
    double opLowHz()    const { return opLow_; }
    double opHighHz()   const { return opHigh_; }
    int    channelId()  const { return channel_; }

    // Task #69 — live WDSP TXA meter taps for the operator-facing
    // MIC / COMP / ALC multimeter sources.  Read the values WDSP
    // computes inside the running TXA chain — safe to poll from
    // the Qt main thread per the reference's GetTXAMeter contract
    // (read the latest stored value, no locking).  All values are
    // already in dB — WDSP's meter.c stores 10·log10(magnitude²)
    // for level meters and 20·log10(linear_gain) for gain meters
    // BEFORE returning via GetTXAMeter (wdsp/meter.c:96-99 ON
    // state; -400.0 / 0.0 sentinels for OFF state at lines 69-72,
    // 103-105).  These accessors are pure pass-throughs — earlier
    // slices double-applied log10 here and pegged MIC at the
    // -200 dB sentinel + railed ALC/COMP bars at full red (Task
    // #71 §1 root cause, fixed 2026-06-02).
    //
    // Lyra TXA dynamics meter set (Task #71 §2; CFC + COMP pruned
    // 2026-06-02 PM — Combinator replaces both per #51 design;
    // Combinator runs as a pre-processor BEFORE the WDSP TXA
    // chain, NOT a WDSP block, so it has no TXA_* meter index —
    // its meters will attach to Lyra-native accessors when #51
    // ships):
    //   Level meters (dBFS, post-stage signal level):
    //     micPeakDbFs     — TXA_MIC_PK  (mic peak into modulator)
    //     levelerPeakDbFs — TXA_LVLR_PK (post-leveler output)
    //     alcPeakDbFs     — TXA_ALC_PK  (post-ALC final output)
    //
    //   Gain meters (dB; positive for LVL boost, negative for
    //   ALC reduction; 0 = no action):
    //     levelerGainDb   — TXA_LVLR_GAIN (leveler boost / red)
    //     alcGainDb       — TXA_ALC_GAIN  (ALC reduction)
    //
    // When the channel isn't open / WDSP not loaded these return
    // the WDSP OFF-state sentinel (-400 dB for level meters,
    // 0 dB for gain meters) so callers can render "—".
    double micPeakDbFs() const;
    double levelerPeakDbFs() const;
    double levelerGainDb() const;
    double alcPeakDbFs() const;
    double alcGainDb() const;

private:
    // Apply the current (mode, opLow_, opHigh_) to the WDSP
    // bandpass + mode setters in the locked freqs-before-mode
    // order.  No-op when the channel isn't open.
    void pushBandpassLocked();

    // Per-mode SSB sign convention.  Operator passes positive
    // edges; the engine maps to baseband signs per the standard
    // SSB convention (USB: l=+low, h=+high; LSB: l=-high, h=-low).
    static std::pair<double, double> signedEdges(Mode m,
                                                 double low,
                                                 double high);

    // Mode → WDSP mode enum int.  USB and LSB only in TX-1.
    static int modeToWdsp(Mode m);

    // Lifecycle guard.  Mirrors the C reference's per-stream
    // pcm->update[stream] critical section: protects the
    // CloseChannel/OpenChannel ↔ fexchange0 race that WDSP's own
    // internal csDSP/csEXCH does NOT cover (those guard the
    // setter ↔ DSP race, not lifecycle teardown).  Held across
    // open()/close()/start()/stop(), across each process() call
    // in TxDspWorker, and briefly inside every operator setter
    // (around the opened_ check + the WDSP setter call — so a
    // close() can't race the setter's WDSP call into a torn-down
    // channel).  Same pattern WdspEngine uses for the RX path.
    mutable std::mutex channelMtx_;

    WdspNative *wdsp_   = nullptr;
    int  channel_       = 1;
    Mode mode_          = Mode::USB;
    double opLow_       = 200.0;
    double opHigh_      = 3100.0;
    int  micRate_       = 48000;
    int  dspRate_       = 96000;
    int  outRate_       = 48000;
    bool opened_        = false;
    // Atomic so the TxDspWorker thread can read via isRunning()
    // without taking channelMtx_ on the hot path.  Writers
    // (open/start/stop/close) hold channelMtx_ AND release-store
    // here; the worker's acquire-load pairs with that store.
    // Task #46 (2026-05-31).
    std::atomic<bool> running_{false};
    // process() hot-path scratch.  Allocated in open() at exactly
    // 2*kInSize doubles each (interleaved I,Q frames).  NEVER
    // resized after open() — process() must not allocate on the
    // RT path that component 4c's MMCSS Pro Audio worker drives.
    std::vector<double> inBuf_;
    std::vector<double> outBuf_;
    int                 fexErr_ = 0;
};

} // namespace lyra::dsp
