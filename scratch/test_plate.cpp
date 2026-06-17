// Unit test for lyra::dsp::Plate (#52).  Qt-free, pure C++.
// Build + run:  cmake --build build --target test_plate
//               build/test_plate.exe
//
// Gates: bypass = untouched; MIX=0 = dry passthrough; an impulse produces a
// decaying reverb tail; a longer DECAY holds more tail energy later; finite
// over a big block at the N8SDR preset (R7/R8).

#include "dsp/Plate.h"

#include <cmath>
#include <cstdio>
#include <vector>

using lyra::dsp::Plate;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

std::vector<double> impulseIQ(int nPairs) {
    std::vector<double> b(2 * nPairs, 0.0);
    b[0] = 1.0;
    return b;
}
std::vector<double> sineIQ(double f, double amp, int nPairs) {
    std::vector<double> b(2 * nPairs, 0.0);
    for (int s = 0; s < nPairs; ++s) b[2 * s] = amp * std::sin(2.0 * kPi * f * s / kFs);
    return b;
}
// Sum of squares of the I channel over [from, to) pairs.
double energy(const std::vector<double> &b, int from, int to) {
    double e = 0.0;
    for (int s = from; s < to; ++s) e += b[2 * s] * b[2 * s];
    return e;
}
}  // namespace

int main() {
    // ── Bypass = untouched ──────────────────────────────────────────────
    {
        Plate p; p.setSampleRate(kFs); p.setBypass(true);
        auto buf = sineIQ(700.0, 0.3, 4096);
        auto ref = buf;
        p.processInterleaved(buf.data(), 4096);
        bool same = true;
        for (size_t i = 0; i < buf.size(); ++i) if (buf[i] != ref[i]) same = false;
        std::printf("bypass untouched=%d\n", same);
        CHECK(same);
    }

    // ── MIX=0 = dry passthrough ─────────────────────────────────────────
    {
        Plate p; p.setSampleRate(kFs); p.setMix(0.0);
        auto buf = sineIQ(700.0, 0.3, 8192);
        auto ref = buf;
        p.processInterleaved(buf.data(), 8192);
        double err = 0.0;
        for (int s = 0; s < 8192; ++s) err = std::max(err, std::fabs(buf[2 * s] - ref[2 * s]));
        std::printf("MIX=0 max err = %.3e\n", err);
        CHECK(err < 1e-9);
    }

    // ── Impulse -> decaying reverb tail (mix=1) ─────────────────────────
    {
        const int N = 96000;   // 2 s
        Plate p; p.setSampleRate(kFs);
        p.setMix(1.0); p.setDecayS(2.0); p.setSize(33.0);
        auto buf = impulseIQ(N);
        p.processInterleaved(buf.data(), N);
        const double eEarly = energy(buf, 480, 4800);     // ~10..100 ms (after pre-delay)
        const double eLate  = energy(buf, 72000, 76320);  // ~1.5 s window
        std::printf("tail early=%.4e late=%.4e\n", eEarly, eLate);
        CHECK(eEarly > 1e-9);          // a tail exists
        CHECK(eLate < eEarly);          // and it decays
    }

    // ── Longer DECAY holds more energy later ────────────────────────────
    {
        const int N = 96000;
        auto run = [&](double dec) {
            Plate p; p.setSampleRate(kFs);
            p.setMix(1.0); p.setDecayS(dec); p.setSize(33.0);
            auto buf = impulseIQ(N);
            p.processInterleaved(buf.data(), N);
            return energy(buf, 46800, 49200);   // ~1.0 s window
        };
        const double eLong  = run(2.5);
        const double eShort = run(0.3);
        std::printf("decay@1s long=%.4e short=%.4e\n", eLong, eShort);
        CHECK(eLong > eShort * 4.0);
    }

    // ── Finite over a big block at the N8SDR preset ─────────────────────
    {
        Plate p; p.setSampleRate(kFs);   // ctor seeds the N8SDR preset
        p.setMix(0.3);
        auto buf = sineIQ(1000.0, 0.4, 96000);
        p.processInterleaved(buf.data(), 96000);
        bool finite = true;
        for (double v : buf) if (!std::isfinite(v)) finite = false;
        std::printf("finite=%d\n", finite);
        CHECK(finite);
    }

    std::printf(g_fail ? "\n%d CHECK(s) FAILED\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
