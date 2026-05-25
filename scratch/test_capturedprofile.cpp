// Standalone capture gate for CapturedProfile (captured-profile slice 2).
// Build (vcvars64 shell, from repo root):
//   cl /std:c++20 /EHsc /O2 /I src scratch\test_capturedprofile.cpp ^
//      src\capturedprofile.cpp src\iqstft.cpp /Fe:scratch\test_cp.exe
//   .\scratch\test_cp.exe
// Gates: (1) a pure complex tone at a known bin makes that bin the
// captured power peak; (2) flat complex noise yields a roughly flat
// profile (no bin wildly dominant); (3) progress/valid lifecycle.

#include "capturedprofile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Minimal deterministic PRNG (LCG) -> uniform [-1,1).
struct Lcg {
    unsigned long long s = 0x12345678ULL;
    double next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (static_cast<double>(s >> 11) / 9007199254740992.0) * 2.0 - 1.0;
    }
};
} // namespace

int main() {
    const int N = 4096, Fs = 192000;
    int fails = 0;

    // ---- (1) tone at bin b dominates ----
    {
        lyra::dsp::CapturedProfile p(N);
        p.begin(Fs, 5.0);
        const int b = 300;
        const double f = static_cast<double>(b) / N;   // cycles/sample
        std::vector<double> chunk(2 * 1024);
        long long n = 0;
        while (p.capturing() && n < 4'000'000) {
            for (int i = 0; i < 1024; ++i) {
                const double ph = 2.0 * kPi * f * static_cast<double>(n + i);
                chunk[2 * i + 0] = std::cos(ph);
                chunk[2 * i + 1] = std::sin(ph);
            }
            p.feed(chunk.data(), 1024);
            n += 1024;
        }
        int peak = 0;
        double pmax = -1.0;
        for (int k = 0; k < N; ++k) {
            if (p.noisePower()[static_cast<size_t>(k)] > pmax) {
                pmax = p.noisePower()[static_cast<size_t>(k)];
                peak = k;
            }
        }
        const bool ok = p.valid() && peak == b;
        std::printf("tone   : valid=%d peakBin=%d (want %d)  [%s]\n",
                    p.valid(), peak, b, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }

    // ---- (2) flat noise -> roughly flat profile ----
    {
        lyra::dsp::CapturedProfile p(N);
        p.begin(Fs, 5.0);
        Lcg rng;
        std::vector<double> chunk(2 * 1024);
        long long n = 0;
        while (p.capturing() && n < 4'000'000) {
            for (int i = 0; i < 2048; ++i) chunk[static_cast<size_t>(i)] = 0.2 * rng.next();
            p.feed(chunk.data(), 1024);
            n += 1024;
        }
        std::vector<double> v = p.noisePower();
        std::sort(v.begin(), v.end());
        const double median = v[v.size() / 2];
        const double pmax = v.back();
        const double ratio = (median > 0.0) ? pmax / median : 1e9;
        const bool ok = p.valid() && ratio < 6.0;   // no wild peak
        std::printf("noise  : valid=%d max/median=%.2f (<6)      [%s]\n",
                    p.valid(), ratio, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }

    // ---- (3) lifecycle: progress climbs, cancel works ----
    {
        lyra::dsp::CapturedProfile p(N);
        p.begin(Fs, 3.0);
        const bool armed = p.capturing() && !p.valid() && p.progress() == 0.0;
        // Feed >= one full frame (N=4096 needs 4096 samples before the
        // first hop fires) so progress can advance.
        std::vector<double> chunk(2 * 8192, 0.0);
        p.feed(chunk.data(), 8192);
        const bool moved = p.progress() > 0.0;
        p.cancel();
        const bool stopped = !p.capturing();
        const bool ok = armed && moved && stopped;
        std::printf("cycle  : armed=%d moved=%d cancel=%d        [%s]\n",
                    armed, moved, stopped, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }

    std::printf("%s\n", fails ? "*** FAIL ***" : "ALL PASS");
    return fails ? 1 : 0;
}
