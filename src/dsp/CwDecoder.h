// Lyra — RX CW (Morse) decoder.  Task #173.
//
// This is a thin adapter around a FAITHFUL, no-drift port of fldigi's CW
// receive chain (src/dsp/cw_fldigi/, ported from fldigi 4.2.06, W1HKJ/VE7IT,
// GPLv3).  The adapter's only jobs are:
//   1. decimate the incoming post-demod RX audio (48 kHz mono, a private copy)
//      down to fldigi's native 8 kHz CW modem rate, and
//   2. forward the operator controls that fldigi actually exposes for RX
//      (tone pitch, bandwidth, speed, tracking, matched filter, squelch), and
//   3. surface the decoded text + receive WPM via callbacks.
//
// There is deliberately NO Lyra front-end here anymore — no IQ slicer, no
// Bayesian classifier, no auto-threshold / narrow / seek machinery.  The
// decode is exactly what fldigi does.  See docs + the memory ledger for the
// 2026-07-10 "faithful fldigi port, no drift" decision.
//
// Threading: the audio thread calls process()/reset(); the UI thread calls the
// setters (plain stores, benign one-block staleness).  Callbacks fire from
// process(); the QObject owner marshals them to the GUI thread.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "dsp/cw_fldigi/fldigi_cw.h"

namespace lyra::dsp {

class CwDecoder {
public:
    CwDecoder();

    // ── live configuration (operator / UI thread) ──────────────────────────
    void setSampleRate(double hz);       // input rate (default 48 kHz)
    void setToneHz(double hz);           // detection centre == the CW pitch
    void setBandwidthHz(int hz);         // fldigi CWbandwidth (Hz), default 150
    void setSpeedWpm(int wpm);           // fldigi CWspeed seed, default 18
    void setTracking(bool on);           // fldigi CWtrack (adaptive speed)
    void setMatchedFilter(bool on);      // fldigi CWmfilt (auto BW = 5*wpm/1.2)
    void setSquelch(bool on, double value); // fldigi sqlonoff + metric threshold

    double toneHz() const { return toneHz_; }

    // ── output callbacks (fire from process()) ──────────────────────────────
    // onText: one decoded unit — a character, a prosign like "<BT>", "*" for an
    //         unrecognised element string, or " " for a word gap (UTF-8).
    // onWpm:  fldigi receive-speed WPM changed.
    std::function<void(const std::string& text)> onText;
    std::function<void(int wpm)>                  onWpm;

    // ── hot path (audio thread) ─────────────────────────────────────────────
    void process(const float* mono, int nframes);
    void reset();

    int rxWpm() const { return rx_.rxWpm(); }

private:
    void rebuildDecimator();

    cwfldigi::CwRx rx_;

    double inRate_  = 48000.0;
    double toneHz_  = 700.0;

    // integer decimating FIR (inRate -> 8 kHz), Hamming-windowed sinc LPF.
    int                 decim_   = 6;      // inRate / 8000
    std::vector<double> firCoef_;          // low-pass taps
    std::vector<double> hist_;             // circular input history
    int                 histPos_ = 0;
    int                 phase_   = 0;      // decimation phase counter
    std::vector<double> out8k_;            // scratch 8 kHz block
};

}  // namespace lyra::dsp
