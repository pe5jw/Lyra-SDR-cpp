// Lyra — IQ-domain STFT / WOLA engine.  See iqstft.h.

#include "iqstft.h"

#include <cmath>

namespace lyra::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

IqStft::IqStft(int fftSize) : n_(fftSize), h_(fftSize / 2) {
    // sqrt of the PERIODIC Hann window.  Periodic (denominator N, not
    // N-1) is what makes Hann sum to a constant 1.0 at 50% overlap; the
    // sqrt split across analysis+synthesis then reconstructs unity.
    win_.resize(static_cast<size_t>(n_));
    for (int k = 0; k < n_; ++k) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * k / n_);
        win_[static_cast<size_t>(k)] = std::sqrt(hann);
    }
    frame_.resize(static_cast<size_t>(n_));
    tail_.assign(static_cast<size_t>(h_), Cplx(0.0, 0.0));
}

void IqStft::reset() {
    inQ_.clear();
    std::fill(tail_.begin(), tail_.end(), Cplx(0.0, 0.0));
}

void IqStft::process(const double *iqInterleaved, int nframes,
                     std::vector<double> &outInterleaved) {
    for (int i = 0; i < nframes; ++i) {
        inQ_.emplace_back(iqInterleaved[2 * i + 0],
                          iqInterleaved[2 * i + 1]);
    }
    // Each full frame consumes one hop of input and emits one hop of
    // output (the front half of the windowed-IFFT frame overlap-added
    // with the previous frame's carried tail).
    while (static_cast<int>(inQ_.size()) >= n_) {
        for (int k = 0; k < n_; ++k) {
            frame_[static_cast<size_t>(k)] =
                inQ_[static_cast<size_t>(k)] * win_[static_cast<size_t>(k)];
        }
        fft(frame_, /*inverse=*/false);
        if (gain_) {
            gain_(frame_);
        }
        fft(frame_, /*inverse=*/true);
        // Emit hop: synthesis-window the front half, add the prior tail.
        for (int k = 0; k < h_; ++k) {
            const Cplx y =
                frame_[static_cast<size_t>(k)] * win_[static_cast<size_t>(k)]
                + tail_[static_cast<size_t>(k)];
            outInterleaved.push_back(y.real());
            outInterleaved.push_back(y.imag());
        }
        // Carry the back half (synthesis-windowed) as the next overlap.
        for (int k = 0; k < h_; ++k) {
            tail_[static_cast<size_t>(k)] =
                frame_[static_cast<size_t>(h_ + k)]
                * win_[static_cast<size_t>(h_ + k)];
        }
        inQ_.erase(inQ_.begin(), inQ_.begin() + h_);
    }
}

void IqStft::fft(std::vector<Cplx> &a, bool inverse) {
    const size_t N = a.size();
    if (N < 2) {
        return;
    }
    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < N; ++i) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    // Danielson-Lanczos butterflies.
    for (size_t len = 2; len <= N; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const Cplx wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < N; i += len) {
            Cplx w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const Cplx u = a[i + k];
                const Cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        const double inv = 1.0 / static_cast<double>(N);
        for (auto &x : a) {
            x *= inv;
        }
    }
}

double IqStft::selfTestMaxError(int fftSize) {
    IqStft st(fftSize);   // identity gain (no GainFn installed)
    // A non-trivial complex test signal: a couple of tones + a ramp so
    // every bin sees energy and reconstruction errors can't hide.
    const int total = fftSize * 8;
    std::vector<double> in;
    in.reserve(static_cast<size_t>(2 * total));
    for (int i = 0; i < total; ++i) {
        const double t = static_cast<double>(i);
        const double re = 0.6 * std::cos(2.0 * kPi * 0.013 * t)
                          + 0.3 * std::cos(2.0 * kPi * 0.21 * t)
                          + 0.05 * (t / total);
        const double im = 0.6 * std::sin(2.0 * kPi * 0.013 * t)
                          - 0.2 * std::sin(2.0 * kPi * 0.07 * t);
        in.push_back(re);
        in.push_back(im);
    }
    std::vector<double> out;
    out.reserve(in.size());
    st.process(in.data(), total, out);
    // Output sample j aligns with input sample j (no extra delay); the
    // first window is warm-up (no prior overlap partner), so compare only
    // the steady-state region past the first FFT frame.
    const int emitted = static_cast<int>(out.size() / 2);
    double maxErr = 0.0;
    for (int j = fftSize; j < emitted; ++j) {
        const double er = std::fabs(out[2 * j + 0] - in[2 * j + 0]);
        const double ei = std::fabs(out[2 * j + 1] - in[2 * j + 1]);
        if (er > maxErr) maxErr = er;
        if (ei > maxErr) maxErr = ei;
    }
    return maxErr;
}

} // namespace lyra::dsp
