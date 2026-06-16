// Unit test for lyra::dsp::SpeechProcessor (#88).  Qt-free, pure C++.
// Build + run:  cmake --build build --target test_speech
//               build/test_speech.exe
//
// Verifies the Auto-AGC leveller pulls quiet/loud toward the target, and
// the De-esser ducks the sibilance band but leaves out-of-band tones alone.

#include "dsp/SpeechProcessor.h"

#include <cmath>
#include <cstdio>
#include <vector>

using lyra::dsp::SpeechProcessor;

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

// Interleaved {I=sine, Q=0} buffer of nPairs pairs.
std::vector<double> sineIQ(double f, double amp, int nPairs) {
    std::vector<double> b(2 * nPairs, 0.0);
    for (int s = 0; s < nPairs; ++s)
        b[2 * s] = amp * std::sin(2.0 * kPi * f * s / kFs);
    return b;
}

// Peak |I| over the tail half.
double peakTail(const std::vector<double> &b, int nPairs) {
    double pk = 0.0;
    for (int s = nPairs / 2; s < nPairs; ++s)
        pk = std::max(pk, std::fabs(b[2 * s]));
    return pk;
}
double rmsTail(const std::vector<double> &b, int nPairs) {
    double sq = 0.0; int m = nPairs - nPairs / 2;
    for (int s = nPairs / 2; s < nPairs; ++s) sq += b[2 * s] * b[2 * s];
    return std::sqrt(sq / m);
}
}  // namespace

int main() {
    const int N = 24000;   // 0.5 s — plenty for the levellers to settle

    // 1) Both stages off = passthrough.
    {
        SpeechProcessor sp; sp.setSampleRate(kFs);
        auto b = sineIQ(1000.0, 0.2, 64);
        const double before = b[2 * 10];
        sp.processInterleaved(b.data(), 64);
        CHECK(std::fabs(b[2 * 10] - before) < 1e-9);
    }

    // 2) Auto-AGC pulls a QUIET signal UP toward the target (-16 dBFS ≈ 0.158).
    {
        SpeechProcessor sp; sp.setSampleRate(kFs);
        sp.setAgcEnabled(true);
        sp.setAgcTargetDb(-16.0); sp.setAgcMaxGainDb(24.0);
        auto b = sineIQ(800.0, 0.05, N);   // quiet input
        sp.processInterleaved(b.data(), N);
        const double pk = peakTail(b, N);
        CHECK(pk > 0.10);          // amplified up from 0.05
        CHECK(pk < 0.26);          // toward, not wildly past, the 0.158 target
    }

    // 3) Auto-AGC pulls a LOUD signal DOWN toward the target.
    {
        SpeechProcessor sp; sp.setSampleRate(kFs);
        sp.setAgcEnabled(true);
        sp.setAgcTargetDb(-16.0); sp.setAgcMaxGainDb(24.0);
        auto b = sineIQ(800.0, 0.6, N);    // loud input
        sp.processInterleaved(b.data(), N);
        const double pk = peakTail(b, N);
        CHECK(pk < 0.30);          // pulled down from 0.6
        CHECK(pk > 0.08);
    }

    // 4) De-esser ducks an in-band (6.5 kHz) tone above threshold.
    {
        SpeechProcessor sp; sp.setSampleRate(kFs);
        sp.setDeessEnabled(true);
        sp.setDeessFreqHz(6500.0); sp.setDeessThreshDb(-30.0);
        sp.setDeessRangeDb(10.0);
        auto b = sineIQ(6500.0, 0.4, N);
        const double in = rmsTail(sineIQ(6500.0, 0.4, N), N);
        sp.processInterleaved(b.data(), N);
        const double out = rmsTail(b, N);
        CHECK(out < 0.75 * in);    // sibilance band clearly attenuated
    }

    // 5) De-esser leaves an OUT-of-band (1 kHz) tone essentially untouched.
    {
        SpeechProcessor sp; sp.setSampleRate(kFs);
        sp.setDeessEnabled(true);
        sp.setDeessFreqHz(6500.0); sp.setDeessThreshDb(-30.0);
        sp.setDeessRangeDb(10.0);
        auto b = sineIQ(1000.0, 0.4, N);
        const double in = rmsTail(sineIQ(1000.0, 0.4, N), N);
        sp.processInterleaved(b.data(), N);
        const double out = rmsTail(b, N);
        CHECK(out > 0.9 * in);     // passband tone barely affected
    }

    if (g_fail == 0) std::printf("test_speech: ALL PASS\n");
    else             std::printf("test_speech: %d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
