// Lyra — IQ-domain STFT / WOLA engine (captured-noise-profile slice 1).
//
// The transform core for Lyra's captured noise profile ("secret weapon"):
// a streaming short-time Fourier transform over complex baseband IQ with
// weighted overlap-add resynthesis.  Slices 2/3 layer capture (per-bin
// magnitude averaging) and apply (Wiener-from-profile gain) on top of the
// per-frame spectrum hook here; this file is JUST the transform + perfect
// reconstruction, with no capture/apply logic and no engine wiring yet.
//
// WHY IQ-domain / pre-WDSP: the noise subtraction must run on the raw IQ
// BEFORE WDSP's RXA chain (and its AGC).  The Python tree burned three
// rounds doing it post-WDSP in the audio domain and every one ticked —
// a static captured noise reference fights WDSP's AGC-modulated live
// floor.  Cleaning the IQ before fexchange0 sidesteps that entirely and
// is mode-independent (the baseband noise is the same regardless of how
// WDSP later demodulates it).
//
// Reconstruction: sqrt-Hann analysis window × sqrt-Hann synthesis window
// = Hann; periodic Hann overlap-added at 50% hop sums to exactly 1.0
// (COLA), so with an identity per-frame gain the output equals the input
// sample-for-sample in steady state (warm-up = first window only).  That
// identity property is the slice-1 correctness gate (selfTestMaxError).
//
// Intentionally dependency-free (no Qt) so it unit-tests standalone and so
// the per-frame math stays portable.  Power-of-two FFT sizes only
// (2048 / 4096 / 8192) — the operator-selectable set.

#pragma once

#include <complex>
#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

namespace lyra::dsp {

class IqStft {
public:
    using Cplx = std::complex<double>;
    // Per-frame spectrum hook: receives the full complex FFT spectrum
    // (length fftSize, bin 0 = DC, standard FFT bin order) and may modify
    // it in place.  Slice 3 installs the Wiener-from-profile gain here;
    // when unset, the transform is identity (perfect reconstruction).
    using GainFn = std::function<void(std::vector<Cplx> &spectrum)>;

    // fftSize MUST be a power of two (2048 / 4096 / 8192).  Hop is
    // fftSize/2 (50% overlap).
    explicit IqStft(int fftSize);

    int  fftSize() const { return n_; }
    int  hop()     const { return h_; }

    // Install / clear the per-frame spectrum modifier.  Safe to change
    // between process() calls.
    void setGain(GainFn fn) { gain_ = std::move(fn); }

    // Clear all streaming state (input FIFO + overlap tail).  Call on a
    // rate/size change or when (re)starting a stream so a new run can't
    // overlap-add onto a stale tail.
    void reset();

    // Stream `nframes` complex samples in as interleaved doubles
    // (I,Q,I,Q,…) and APPEND the resynthesized (cleaned) samples to
    // `outInterleaved` in the same interleaved format.  Output is
    // emitted a hop at a time as full frames complete, so the number of
    // samples appended generally differs from nframes (it lags by up to
    // one frame).  Over a long run, total-out == total-in minus the
    // final partial window still buffered.  Single-threaded.
    void process(const double *iqInterleaved, int nframes,
                 std::vector<double> &outInterleaved);

    // Slice-1 correctness gate: run a known signal through an
    // identity-gain IqStft and return the maximum |out-in| over the
    // steady-state region (past the first window).  A COLA-correct WOLA
    // returns ~1e-12.  Static so it builds/runs with no engine.
    static double selfTestMaxError(int fftSize);

private:
    // In-place iterative radix-2 FFT.  inverse=true divides by N.
    static void fft(std::vector<Cplx> &a, bool inverse);

    int                 n_ = 0;     // FFT size (power of two)
    int                 h_ = 0;     // hop = n_/2
    std::vector<double> win_;       // sqrt-Hann (analysis == synthesis)
    std::vector<Cplx>   frame_;     // scratch FFT frame (length n_)
    std::vector<Cplx>   tail_;      // overlap-add carry (length h_)
    std::deque<Cplx>    inQ_;       // samples awaiting framing
    GainFn              gain_;      // per-frame spectrum modifier (or null)
};

} // namespace lyra::dsp
