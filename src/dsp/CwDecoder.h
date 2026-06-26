// Lyra — native real-time Morse (CW) decoder.  Task #173 (CW-5a).
//
// Decodes a single CW tone out of narrowband receive audio.  Qt-free,
// allocation-light on the hot path, fully unit-testable against synthetic
// CW.  The signal chain (a faithful port of the operator's own proven
// decoder; described here in first-principles DSP terms):
//
//   tone-frequency IQ downconvert (coherent quadrature NCO) -> magnitude
//   -> optional impulse noise blanker
//   -> asymmetric envelope follower (fast attack / slow decay)
//   -> matched filter (running mean ~0.3 dit, window adapts to speed)
//   -> 3-mode adaptive noise-floor + peak/QSB tracker
//   -> SNR squelch + proportional-hysteresis slicer  -> mark / space edges
//   -> one-edge lookback merge (rejects noise blips + threshold flicker)
//   -> Bayesian dit/dah mark classifier + Farnsworth space classifier,
//      both with online "fist" learning  -> Morse element string -> char.
//
// A periodic Goertzel AFC sweep (±range about the configured tone) tracks
// carrier drift and re-centres the downconvert NCO, with lock hysteresis so
// it won't hop between two signals in the passband.  All amplitude handling
// is internally normalised (noise-floor / peak), so absolute input level is
// irrelevant — feed mono blocks at the configured sample rate and read the
// decode via the callbacks.
//
// Threading: a single producer (the audio thread) calls process()/reset();
// the operator/UI thread calls the setters.  The scalar setters are plain
// stores (benign one-block staleness on the hot path); there is no lock.
// The callbacks fire from process() — the QObject wrapper that owns this
// (CW-5b) is responsible for marshalling them to the GUI thread.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lyra::dsp {

class CwDecoder {
public:
    // Matched-filter ring capacity, in IQ windows (~0.7 ms each).
    static constexpr int kMfMax = 256;

    CwDecoder();

    // ── live configuration (operator / UI thread) ──────────────────────────
    void setSampleRate(double hz);     // default 48 kHz
    void setToneHz(double hz);         // detection centre == the CW pitch
    void setSquelch(double snr);       // gate on when peak >= snr * noiseFloor
    void setThreshold(double frac01);  // slicer level 0..1 (UI percent / 100)
    void setAfcEnabled(bool on);
    void setAfcRange(int hz);          // ± search range (50/100/150/200)
    void setDspFilter(bool on);        // tighter matched-filter window
    void setNoiseBlanker(bool on);
    void setTxWpm(int wpm);            // bootstrap seed for the timing model

    double toneHz()  const { return toneHz_; }

    // ── output callbacks (fire from process()) ──────────────────────────────
    // onChar: one decoded character; '?' for an unrecognised element string;
    //         ' ' for a word gap.  confidence is an advisory 0..1 (1 = certain;
    //         word gaps are 1; '?' is 0; nearest-code rescues and weak/ambiguous
    //         decodes read low) — purely additive, the decode itself is unchanged.
    // onWpm:  adaptive receive speed changed (1200 / dit-ms).
    // onAfc:  AFC lock state and/or locked frequency changed.
    std::function<void(char ch, double confidence)> onChar;
    std::function<void(int wpm)>                onWpm;
    std::function<void(bool locked, double hz)> onAfc;

    // ── hot path (audio thread) ─────────────────────────────────────────────
    void process(const float* mono, int nframes);
    void reset();   // clear all DSP/timing/decode state; re-seed from txWpm

    int    rxWpm()     const { return rxWpm_; }
    bool   afcLocked() const { return afcLocked_; }
    double afcHz()     const { return afcHz_.value_or(toneHz_); }

private:
    enum class Space { Elem, Char, Word };

    double goertzel(const float* s, int n, double targetHz) const;
    void   runAfc(const float* s, int n, double centerHz);
    void   updateAfcState(bool locked, double hz);
    static double logGaussian(double x, double mu, double varr);
    bool   bayesClassifyMark(double ms) const;
    Space  bayesClassifySpace(double ms) const;
    void   bayesLearnMark(double ms, bool isDit);
    void   submitEdge(bool wasMark, double durationMs);
    void   processEdge(bool wasMarkBefore, double durationMs);
    void   flushChar();
    void   appendDecoded(char ch, double confidence);
    void   seedTiming();   // (re)init dotEst / dit model / element-space from txWpm

    // ── A-slice accuracy helpers (additive — never alter the element string) ──
    char   rescueNearest(const std::string& code) const;  // A1: unique dist-1 char or '\0'
    double markConfidence(double ms) const;               // A3: dit/dah decision margin → 0..1
    double snrConfidence() const;                          // A3: decode-time SNR → 0..1
    double charConfidence(bool rescued) const;             // A3: blended per-character 0..1

    // ── configuration ──
    double sampleRate_  = 48000.0;
    double toneHz_      = 700.0;
    double squelch_     = 1.5;
    double threshold_   = 0.35;
    bool   afcEnabled_  = true;
    int    afcRange_    = 100;
    bool   dspFilter_   = false;
    bool   noiseBlanker_= false;
    int    txWpm_       = 20;

    // ── impulse blanker ──
    double nbRunAvg_ = 0.0;

    // ── AFC ──
    std::optional<double> afcHz_;
    std::optional<double> afcPrevHz_;
    int                   afcLockCount_ = 0;
    std::vector<float>    afcBuf_;
    int                   afcCount_  = 0;
    bool                  afcLocked_ = false;

    // ── floor holdoff / QSB ──
    bool   qsbFadeFlag_   = false;
    double floorHoldoffEnd_ = 0.0;
    bool   snrGateOpen_   = false;   // A6: hysteretic SNR-gate latch

    // ── Farnsworth (independent space centres) ──
    double bCharSpaceMu_ = 0.0;
    double bWordSpaceMu_ = 0.0;

    // ── IQ downconvert ──
    double iqPhaseAcc_ = 0.0;
    double iqI_ = 0.0, iqQ_ = 0.0;
    int    iqCount_ = 0;
    double envVal_ = 0.0;

    // ── matched filter ──
    std::array<double, kMfMax> mfBuf_{};
    int    mfPos_ = 0;
    double mfSum_ = 0.0;
    int    mfWin_ = 32, mfWinLast_ = 0;

    // ── noise floor / peak ──
    double noiseFloor_ = 0.0, peakPower_ = 0.0;
    int    peakSnapHold_ = 0;   // A5: windows the elevated level has persisted

    // ── timing / decode state ──
    double dotEstMs_  = 60.0;
    double lastEdge_  = 0.0;
    bool   lastLevel_ = false;
    std::string currentChar_;
    std::optional<bool> bufWasMark_;
    double bufDur_      = 0.0;
    double sampleTimeMs_= 0.0;

    // ── Bayesian dit/dah model ──
    double bDitMu_  = 60.0;
    double bDitVar_ = 0.0;
    double bDahMu_  = 180.0;  // B1: dah length learned INDEPENDENTLY (≈3×dit),
                              // not folded ms/3 into the dit estimate — so a
                              // light fist no longer biases dotEst/WPM.
    int    bMarkN_  = 0;
    double bElemSpaceMu_ = 0.0;

    // ── per-character confidence accumulators (A3) ──
    double charConfSum_ = 0.0;   // Σ dit/dah decision margins over the char
    double charSnrSum_  = 0.0;   // Σ decode-time SNR samples over the char
    int    charConfN_   = 0;     // mark count contributing to the two sums

    // ── output bookkeeping ──
    char   lastEmitted_ = '\0';
    int    rxWpm_ = 0;
};

} // namespace lyra::dsp
