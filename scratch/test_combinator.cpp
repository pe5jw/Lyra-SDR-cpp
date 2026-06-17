// Unit test for lyra::dsp::Combinator (#51).  Qt-free, pure C++.
// Build + run:  cmake --build build --target test_combinator
//               build/test_combinator.exe
//
// Gates the design-note red-team items:
//   R1  LR4 5-way crossover reconstructs to ~unity (bands disabled => flat).
//   --  per-band compressor produces gain reduction on a loud in-band tone.
//   --  MIX=0 => dry passthrough; MIX=1 => fully processed.
//   --  SBC engages without blowing up.
//   R7  no NaN/Inf over a large block.

#include "dsp/Combinator.h"

#include <cmath>
#include <cstdio>
#include <vector>

using lyra::dsp::Combinator;

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

std::vector<double> sineIQ(double f, double amp, int nPairs) {
    std::vector<double> b(2 * nPairs, 0.0);
    for (int s = 0; s < nPairs; ++s)
        b[2 * s] = amp * std::sin(2.0 * kPi * f * s / kFs);
    return b;
}
double peakTail(const std::vector<double> &b, int nPairs) {
    double pk = 0.0;
    for (int s = nPairs / 2; s < nPairs; ++s)
        pk = std::max(pk, std::fabs(b[2 * s]));
    return pk;
}
double dB(double r) { return 20.0 * std::log10(r + 1e-12); }
}  // namespace

int main() {
    const int N = 14400;   // 0.3 s — well past the slowest crossover settle

    // ── R1: crossover reconstruction (all bands disabled => pass flat) ──
    {
        const double freqs[] = { 50, 120, 300, 700, 2000, 5000, 9000 };
        for (double f : freqs) {
            Combinator c;
            c.setSampleRate(kFs);
            c.setSbcEnabled(false);
            c.setMix(1.0);
            for (int b = 0; b < Combinator::kNumBands; ++b) c.setBandEnabled(b, false);
            const double amp = 0.2;
            auto buf = sineIQ(f, amp, N);
            c.processInterleaved(buf.data(), N);
            const double r = peakTail(buf, N) / amp;
            std::printf("R1  %5.0f Hz  recon %+.2f dB (x%.3f)\n", f, dB(r), r);
            // < 1 dB ripple across the audio band at unity.
            CHECK(dB(r) > -1.0 && dB(r) < 1.0);
        }
    }

    // ── Compressor: loud in-band tone => gain reduction; quiet => none ──
    {
        // 700 Hz lands in MID (band 2, between 500 and 1500 after X-OVER).
        Combinator c;
        c.setSampleRate(kFs);
        c.setSbcEnabled(false);
        c.setMix(1.0);
        c.setThreshDb(-30.0);
        // Loud tone, well above threshold.
        auto loud = sineIQ(700.0, 0.5, N);   // -6 dBFS
        c.processInterleaved(loud.data(), N);
        double grLoud = 0.0;
        for (int b = 0; b < Combinator::kNumBands; ++b)
            grLoud = std::max(grLoud, c.bandReductionDb(b));
        std::printf("COMP loud GR = %.2f dB\n", grLoud);
        CHECK(grLoud > 1.0);   // meaningfully compressing

        // Quiet tone, below threshold => no reduction.
        Combinator c2;
        c2.setSampleRate(kFs);
        c2.setSbcEnabled(false);
        c2.setThreshDb(-30.0);
        auto quiet = sineIQ(700.0, 0.003, N);   // ~-50 dBFS
        c2.processInterleaved(quiet.data(), N);
        double grQuiet = 0.0;
        for (int b = 0; b < Combinator::kNumBands; ++b)
            grQuiet = std::max(grQuiet, c2.bandReductionDb(b));
        std::printf("COMP quiet GR = %.2f dB\n", grQuiet);
        CHECK(grQuiet < 0.5);
    }

    // ── MIX: 0 => dry passthrough, 1 => processed (compressed) ──
    {
        const double amp = 0.5;
        Combinator dryC;
        dryC.setSampleRate(kFs);
        dryC.setSbcEnabled(false);
        dryC.setThreshDb(-30.0);
        dryC.setMix(0.0);
        auto dry = sineIQ(700.0, amp, N);
        dryC.processInterleaved(dry.data(), N);
        const double rDry = peakTail(dry, N) / amp;
        std::printf("MIX=0 passthrough %+.2f dB\n", dB(rDry));
        CHECK(dB(rDry) > -0.2 && dB(rDry) < 0.2);   // unaffected

        Combinator wetC;
        wetC.setSampleRate(kFs);
        wetC.setSbcEnabled(false);
        wetC.setThreshDb(-30.0);
        wetC.setMix(1.0);
        auto wet = sineIQ(700.0, amp, N);
        wetC.processInterleaved(wet.data(), N);
        const double rWet = peakTail(wet, N) / amp;
        std::printf("MIX=1 processed   %+.2f dB\n", dB(rWet));
        CHECK(dB(rWet) < dB(rDry) - 0.5);           // visibly more compressed
    }

    // ── SBC: engages, stays bounded ──
    {
        Combinator c;
        c.setSampleRate(kFs);
        c.setSbcEnabled(true);
        c.setSbcSpeed(8.0);
        auto buf = sineIQ(2000.0, 0.3, N);   // energy in one band only
        c.processInterleaved(buf.data(), N);
        bool bounded = true, active = false;
        for (int b = 0; b < Combinator::kNumBands; ++b) {
            const double s = c.bandSbcDb(b);
            if (!std::isfinite(s) || std::fabs(s) > 6.001) bounded = false;
            if (std::fabs(s) > 0.01) active = true;
        }
        std::printf("SBC bounded=%d active=%d\n", bounded, active);
        CHECK(bounded);
        CHECK(active);
    }

    // ── R7: no NaN/Inf over a big block at the default N8SDR preset ──
    {
        Combinator c;
        c.setSampleRate(kFs);   // ctor already seeds the N8SDR preset
        auto buf = sineIQ(1000.0, 0.4, N);
        c.processInterleaved(buf.data(), N);
        bool finite = true;
        for (double v : buf) if (!std::isfinite(v)) finite = false;
        std::printf("R7  all-finite=%d\n", finite);
        CHECK(finite);
    }

    std::printf(g_fail ? "\n%d CHECK(s) FAILED\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
