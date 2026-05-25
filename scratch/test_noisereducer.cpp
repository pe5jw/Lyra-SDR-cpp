// Standalone apply gate for NoiseReducer (captured-profile slice 3).
// Build (vcvars64 shell, repo root):
//   cl /std:c++20 /EHsc /O2 /I src scratch\test_noisereducer.cpp ^
//      src\noisereducer.cpp src\capturedprofile.cpp src\iqstft.cpp ^
//      /Fe:scratch\test_nr.exe
//   .\scratch\test_nr.exe
// Gates: (A) noise-only floor drops by >= 6 dB through a profile captured
// on the same noise; (B) a strong tone is preserved (gain ~1 where signal
// dominates); (C) with no profile installed the reducer is exact identity
// (output == input delayed by one window).

#include "capturedprofile.h"
#include "noisereducer.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int    N   = 4096;
constexpr int    Fs  = 192000;
constexpr double kNoise = 0.30;

struct Lcg {
    unsigned long long s;
    explicit Lcg(unsigned long long seed) : s(seed) {}
    double next() {   // uniform [-1,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (static_cast<double>(s >> 11) / 9007199254740992.0) * 2.0 - 1.0;
    }
};

double rms(const std::vector<double> &iq, int fromFrame, int toFrame) {
    double acc = 0.0;
    int n = 0;
    for (int f = fromFrame; f < toFrame; ++f) {
        const double re = iq[static_cast<size_t>(2 * f)];
        const double im = iq[static_cast<size_t>(2 * f + 1)];
        acc += re * re + im * im;
        ++n;
    }
    return (n > 0) ? std::sqrt(acc / n) : 0.0;
}

// Magnitude of the complex tone at cycles/sample `f` over [from,to).
double toneMag(const std::vector<double> &iq, double f, int from, int to) {
    std::complex<double> acc(0.0, 0.0);
    int n = 0;
    for (int k = from; k < to; ++k) {
        const std::complex<double> x(iq[static_cast<size_t>(2 * k)],
                                     iq[static_cast<size_t>(2 * k + 1)]);
        const double ph = -2.0 * kPi * f * static_cast<double>(k);
        acc += x * std::complex<double>(std::cos(ph), std::sin(ph));
        ++n;
    }
    return std::abs(acc) / (n > 0 ? n : 1);
}
} // namespace

int main() {
    int fails = 0;

    // Capture a noise profile on white noise of scale kNoise.
    lyra::dsp::CapturedProfile cap(N);
    cap.begin(Fs, 5.0);
    {
        Lcg rng(1);
        std::vector<double> chunk(2 * 1024);
        long long n = 0;
        while (cap.capturing() && n < 4'000'000) {
            for (int i = 0; i < 2048; ++i) chunk[static_cast<size_t>(i)] = kNoise * rng.next();
            cap.feed(chunk.data(), 1024);
            n += 1024;
        }
    }
    const std::vector<double> profile = cap.noisePower();

    // ---- (A) noise-only floor reduction ----
    {
        lyra::dsp::NoiseReducer nr(N);
        nr.setProfile(profile);                 // defaults: a=1, floor -12 dB, s=0.6
        Lcg rng(999);                            // fresh realization, same stats
        std::vector<double> in, out;
        std::vector<double> chunk(2 * 1024), ob(2 * 1024);
        const int chunks = 400;
        for (int c = 0; c < chunks; ++c) {
            for (int i = 0; i < 2048; ++i) chunk[static_cast<size_t>(i)] = kNoise * rng.next();
            in.insert(in.end(), chunk.begin(), chunk.end());
            nr.process(chunk.data(), 1024, ob.data());
            out.insert(out.end(), ob.begin(), ob.end());
        }
        const int total = chunks * 1024;
        const double ri = rms(in,  2 * N, total);
        const double ro = rms(out, 2 * N, total);
        const double dropDb = 20.0 * std::log10(ri / (ro > 0 ? ro : 1e-12));
        const bool ok = dropDb >= 6.0;
        std::printf("reduce : noise floor drop = %.2f dB (>=6)   [%s]\n",
                    dropDb, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }

    // ---- (B) tone preservation ----
    {
        lyra::dsp::NoiseReducer nr(N);
        nr.setProfile(profile);
        const int b = 512;
        const double f = static_cast<double>(b) / N;
        Lcg rng(7);
        std::vector<double> in, out;
        std::vector<double> chunk(2 * 1024), ob(2 * 1024);
        const int chunks = 400;
        long long s = 0;
        for (int c = 0; c < chunks; ++c) {
            for (int i = 0; i < 1024; ++i) {
                const double ph = 2.0 * kPi * f * static_cast<double>(s + i);
                chunk[static_cast<size_t>(2 * i)]     = std::cos(ph) + kNoise * rng.next();
                chunk[static_cast<size_t>(2 * i + 1)] = std::sin(ph) + kNoise * rng.next();
            }
            s += 1024;
            in.insert(in.end(), chunk.begin(), chunk.end());
            nr.process(chunk.data(), 1024, ob.data());
            out.insert(out.end(), ob.begin(), ob.end());
        }
        const int total = chunks * 1024;
        const double ai = toneMag(in,  f, 2 * N, total);
        const double ao = toneMag(out, f, 2 * N, total);
        const double ratio = ao / (ai > 0 ? ai : 1e-12);
        const bool ok = ratio > 0.90 && ratio < 1.05;
        std::printf("tone   : out/in amplitude = %.3f (0.90-1.05) [%s]\n",
                    ratio, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }

    // ---- (C) no profile -> exact identity, delayed by one window ----
    {
        lyra::dsp::NoiseReducer nr(N);           // no setProfile -> not ready
        Lcg rng(3);
        std::vector<double> in, out;
        std::vector<double> chunk(2 * 1024), ob(2 * 1024);
        const int chunks = 60;
        for (int c = 0; c < chunks; ++c) {
            for (int i = 0; i < 2048; ++i) chunk[static_cast<size_t>(i)] = 0.5 * rng.next();
            in.insert(in.end(), chunk.begin(), chunk.end());
            nr.process(chunk.data(), 1024, ob.data());
            out.insert(out.end(), ob.begin(), ob.end());
        }
        const int total = chunks * 1024;
        double maxErr = 0.0;
        // out[m] == in[m-N] (one-window prime delay) in steady state.
        for (int m = 2 * N; m < total; ++m) {
            const double er = std::fabs(out[static_cast<size_t>(2 * m)]
                                        - in[static_cast<size_t>(2 * (m - N))]);
            const double ei = std::fabs(out[static_cast<size_t>(2 * m + 1)]
                                        - in[static_cast<size_t>(2 * (m - N) + 1)]);
            if (er > maxErr) maxErr = er;
            if (ei > maxErr) maxErr = ei;
        }
        const bool ok = maxErr < 1e-9;
        std::printf("ident  : max |out-in(delayed)| = %.3e (<1e-9) [%s]\n",
                    maxErr, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }

    std::printf("%s\n", fails ? "*** FAIL ***" : "ALL PASS");
    return fails ? 1 : 0;
}
