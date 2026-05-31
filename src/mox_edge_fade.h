// Lyra-cpp — TX-1 component 5a: MOX-edge cos² amplitude envelope.
//
// 50 ms cos² fade-in / 13 ms cos² fade-out (defaults; both operator-
// tunable) applied to TX I/Q amplitude at the EP2 wire-pack stage.
// Composes ON TOP of the TX-0c TR-sequencing FSM:
//
//   ╔════════════════════════════════════════════════════════════╗
//   ║   KEYDOWN timeline (operator hits PTT at T=0):             ║
//   ║                                                            ║
//   ║   T=0     operator → requestMox(true)                      ║
//   ║   T=0     ATT-on-TX raised (31 dB step-att, RX-protect)    ║
//   ║   T=0     OC pattern flipped to TX-side band filters       ║
//   ║   T=15ms  wire MOX bit hot on next datagram                ║
//   ║   T=15ms  ◀── MoxEdgeFade fade-in STARTS (coef 0 → 1)      ║
//   ║   T=65ms  ◀── fade-in COMPLETES (coef = 1.0)               ║
//   ║   T=65ms  moxActiveChanged(true) — "RF settled, on air"    ║
//   ║                                                            ║
//   ║   KEYUP timeline (operator releases PTT at T=0):           ║
//   ║                                                            ║
//   ║   T=0     operator → requestMox(false)                     ║
//   ║   T=0     space_mox re-key window opens                    ║
//   ║   T=0     ◀── fade-out STARTS (coef 1.0 → 0)               ║
//   ║   T=13ms  ◀── fade-out COMPLETES (coef = 0.0)              ║
//   ║   T=13ms  wire MOX bit clears — gateware stops             ║
//   ║          consuming TX I/Q (per ak4951v4 gateware:          ║
//   ║          dsopenhpsdr1.v:351-371, TX-IQ → DAC only          ║
//   ║          while wire MOX bit is set).                       ║
//   ║   T=18ms  step-att restored, OC RX pattern, moxActive→0    ║
//   ╚════════════════════════════════════════════════════════════╝
//
// THE LOAD-BEARING SAFETY POINT (operator-mandated, 2026-05-31):
//
// This envelope is NOT click-cosmetics.  It is hot-switch protection
// for external solid-state HF linear amplifiers.  An SS linear's T/R
// relay needs ~30-50 ms to fully settle; applying RF into a mid-
// transition relay can destroy the PA.  Two layers protect against
// this in lyra-cpp:
//
//   1. TR-sequencing rf_delay (50 ms default) — wire MOX hot, wait,
//      THEN start producing RF.  Schedules the relay-settle window.
//
//   2. MoxEdgeFade fade-in (50 ms default) — even WITHIN the rf_delay
//      window, the host-side I/Q amplitude ramps smoothly from 0.
//      Belt-and-suspenders: redundant protection against any residual
//      MOX-bit / relay timing skew.  Default fade-in duration matches
//      rf_delay duration so the ramp completes EXACTLY when "RF
//      settled" is emitted.
//
// The asymmetric default (50 ms in / 13 ms out) reflects the asymmetric
// risk: RF coming UP into a still-switching relay can kill amps; RF
// coming DOWN doesn't damage anything (the relay disengages cleanly).
// Fade-OUT exists only to prevent the AUDIBLE click at the host →
// gateware DAC transition; it MUST complete BEFORE wire-MOX clears,
// or the gateware stops consuming TX-IQ mid-ramp (= residual hard
// step on the air).  Fade-out is therefore sized to fit inside the
// space_mox window (13 ms default).
//
// SOLE envelope for SSB voice TX — the WDSP cos² envelope
// (wdsp/uslew.c) does NOT envelope-shape SSB output (verified per
// docs/architecture/tx1_ssb_design.md §4.9 against wdsp/uslew.c +
// wdsp/TXA.c TXAUslewCheck; uslew only arms for ammod/fmmod/gen0/
// gen1, all OFF in SSB modes).  This envelope is the only thing
// protecting against external-amp damage AND the only thing
// preventing the keydown click for SSB voice.
//
// MID-FADE REVERSAL CONTINUITY:
// If the operator dekeys while fade-in is still rising, or re-keys
// while fade-out is still falling, the coefficient must NOT jump.
// Cos² complement-of-phase preserves the coef across the state flip:
//   FadingIn at phase k_in / N_in → coef_in
//   solve coef_in == coef_out(k_out, N_out) for k_out:
//     0.5*(1 - cos(π*k_in/N_in)) == 0.5*(1 + cos(π*k_out/N_out))
//     cos(π*k_out/N_out) = -cos(π*k_in/N_in) = cos(π*(1 - k_in/N_in))
//     k_out = N_out * (1 - k_in/N_in)
// Same in the other direction (swap in↔out).  Smooth pivot, no click.
//
// THREAD MODEL:
//   * notifyMoxState() + advance() called from ONE thread only — the
//     EP2 wire writer thread.  No locks needed for the hot path.
//   * setFadeInMs() + setFadeOutMs() called from the Qt main thread
//     (operator Settings UI).  Durations are std::atomic<int> so the
//     writer thread reads coherently without locks.
//   * isIdle()/isOn() are diagnostic getters (logging only); reading
//     state_ from another thread may observe a momentary inconsistency
//     but is harmless — never used in a safety decision.

#pragma once

#include <atomic>
#include <cstdint>

namespace lyra::dsp {

class MoxEdgeFade {
public:
    // EP2 audio rate is fixed at 48 kHz (HL2 AK4951 hardware lock).
    static constexpr int kSampleRateHz = 48000;

    // Defaults: 50 ms fade-in (aligns with the hot-switch-safe
    // rf_delay = 50 ms default — see hl2_stream.h kRfDelayMs), 13 ms
    // fade-out (fits inside space_mox = 13 ms before wire-MOX clears).
    static constexpr int kDefaultFadeInMs  = 50;
    static constexpr int kDefaultFadeOutMs = 13;

    // Operator-tuning bounds.  1 ms reduces to the hard-step click;
    // 500 ms is unphysical (would outlast typical PTT events).
    static constexpr int kMinFadeMs = 1;
    static constexpr int kMaxFadeMs = 500;

    MoxEdgeFade();

    // Operator-tunable durations.  Clamped to [kMinFadeMs, kMaxFadeMs].
    // Thread-safe (atomic).  Setting a duration mid-fade takes effect
    // on the NEXT MOX edge; the in-flight fade finishes at whatever
    // rate it started.
    void setFadeInMs(int ms) noexcept;
    void setFadeOutMs(int ms) noexcept;
    int  fadeInMs()  const noexcept;
    int  fadeOutMs() const noexcept;

    // Call ONCE per datagram with the wire-MOX snapshot (the same
    // moxBit value the EP2 packer uses to gate emitTone / TX I/Q
    // output).  Edge-detects MOX changes and drives state transitions.
    // No-op if MOX hasn't changed since the previous call.
    void notifyMoxState(bool moxOn) noexcept;

    // Call ONCE per LRIQ sample (inside the EP2 packer's i=0..125
    // loop, both USB frames).  Returns the cos² envelope coefficient
    // in [0.0, 1.0] and ticks the internal phase by one sample.
    //
    // Caller multiplies this against TX I/Q before quantizing to
    // int16 for the wire frame:
    //
    //   const float coef = fade_.advance();
    //   const qint16 txI = static_cast<qint16>(rawTxI * coef + 0.5f);
    //
    // Cost: one std::cosf() per call (~50 ns on modern hardware) +
    // one atomic_load (free on x86).  Total budget ≈ 380 dg/s × 126
    // samples × 50 ns ≈ 2.4 ms CPU per second.  Negligible vs WDSP.
    float advance() noexcept;

    // Diagnostics — safe to call from any thread, but observed value
    // may briefly disagree with the writer thread's true state under
    // a concurrent edge transition.  Logging only.
    bool isIdle() const noexcept;
    bool isOn()   const noexcept;

private:
    enum class State : std::uint8_t {
        Idle,       // coef = 0, no TX
        FadingIn,   // coef rising from current value toward 1
        On,         // coef = 1, full TX
        FadingOut   // coef falling from current value toward 0
    };

    static constexpr int msToSamples(int ms) noexcept {
        return (ms * kSampleRateHz) / 1000;
    }

    static int clampMs(int ms) noexcept;

    // Operator-tunable durations stored in SAMPLES (not ms) so the
    // hot path avoids the ms↔samples conversion per call.
    std::atomic<int> fadeInSamples_{msToSamples(kDefaultFadeInMs)};
    std::atomic<int> fadeOutSamples_{msToSamples(kDefaultFadeOutMs)};

    // Writer-thread-only state.
    std::atomic<State> state_{State::Idle};  // atomic for diagnostic readers
    int                phase_   = 0;
    bool               prevMox_ = false;
};

} // namespace lyra::dsp
