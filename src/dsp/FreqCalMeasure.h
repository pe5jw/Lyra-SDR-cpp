// Lyra — frequency-calibration carrier measurement (WWV / time-station).
// Task: frequency calibration, Stage 3 (freq_calibration_design.md).
//
// Estimates the AUDIO frequency of a steady carrier tone to a fraction of a
// hertz.  Fed the SAME mono RX audio the CW decoder taps (wdsp_engine), but
// only while a measurement is armed.  A time-station carrier, tuned so it
// lands near a known target audio pitch, appears as one dominant tone; this
// class locates it (Goertzel coarse scan + parabolic sub-bin), gates on
// carrier strength (peak / noise-floor SNR), and averages over windows so the
// reading settles.  Qt-free, allocation-light on the hot path, unit-testable.
//
// This class ONLY answers "what audio frequency is the carrier at, and is it
// strong enough to trust?".  The correction math (offset -> factor) and the
// tuning setup live at the coordinator (HL2Stream), where the sign convention
// is pinned and bench-verified on live WWV.
#pragma once

#include <vector>

namespace lyra::dsp {

class FreqCalMeasure {
public:
    void setSampleRate(double hz);
    // Expected carrier audio pitch + half-width of the search window (Hz).
    // The coordinator tunes so the carrier lands near targetHz.
    void setTarget(double targetHz, double halfRangeHz);
    void setSnrGateDb(double db);        // strong() threshold (default 10 dB)
    void setWindowSize(int n);           // analysis window (default 8192)
    void setRequiredWindows(int n);      // strong windows for ready() (default 12)

    void start();                        // arm + clear accumulators
    void process(const float* mono, int n);  // hot path (audio thread)

    // Results (UI / coordinator thread — plain scalar reads).
    bool   ready()      const { return strongN_ >= reqWindows_; }
    double measuredHz() const { return measuredHz_; }
    double offsetHz()   const { return measuredHz_ - targetHz_; }
    double snrDb()      const { return snrDb_; }
    bool   strong()     const { return snrDb_ >= snrGateDb_; }
    int    windows()    const { return strongN_; }   // strong windows only
    int    analyzed()   const { return analyzedN_; } // every window (live tick)
    double targetHz()   const { return targetHz_; }

private:
    void analyzeWindow_();

    double sampleRate_ = 48000.0;
    double targetHz_   = 1000.0;
    double halfRange_  = 400.0;
    double snrGateDb_  = 10.0;
    int    winSize_    = 8192;
    int    reqWindows_ = 12;

    std::vector<float> buf_;   // raw accumulation buffer (Hann applied in analyze)
    int    fill_ = 0;

    double measuredHz_ = 0.0;  // EWMA of strong-window peak frequencies
    double snrDb_      = 0.0;   // last window peak / noise floor
    int    strongN_    = 0;     // strong windows contributing
    int    analyzedN_  = 0;     // total windows analyzed (live tick)
    bool   haveEwma_   = false;
};

} // namespace lyra::dsp
