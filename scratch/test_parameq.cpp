// Unit test for lyra::dsp::ParamEq (#50 / #59).  Qt-free, pure C++.
// Build + run:  cmake --build build --target test_parameq
//               build/test_parameq.exe
//
// Verifies (a) the RBJ cookbook math (peak gain at centre, shelf/LP/HP
// behaviour, bypass + disabled = unity), and (b) the load-bearing
// invariant: the magnitude the UI draws via magnitudeDb() equals the
// gain process() actually applies to a steady sine — "what you see is
// what you hear".

#include "dsp/ParamEq.h"

#include <cmath>
#include <cstdio>
#include <vector>

using lyra::dsp::ParamEq;

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void disableAll(ParamEq &eq) {
    for (int i = 0; i < ParamEq::kNumBands; ++i) {
        ParamEq::Band b = eq.band(i);
        b.enabled = false;
        eq.setBand(i, b);
    }
}

// Measured steady-state gain (dB) process() applies to a sine at f.
double processGainDb(ParamEq &eq, double f) {
    const int N = 8192;
    std::vector<float> buf(N);
    for (int n = 0; n < N; ++n)
        buf[n] = static_cast<float>(0.5 * std::sin(2.0 * kPi * f * n / kFs));
    double inSq = 0.0;
    for (int n = N / 2; n < N; ++n) {
        const double x = 0.5 * std::sin(2.0 * kPi * f * n / kFs);
        inSq += x * x;
    }
    eq.reset();
    eq.process(buf.data(), N);  // first call publishes staged coeffs
    double outSq = 0.0;
    for (int n = N / 2; n < N; ++n)
        outSq += static_cast<double>(buf[n]) * buf[n];
    const int m = N / 2;
    return 20.0 * std::log10(std::sqrt(outSq / m) / std::sqrt(inSq / m));
}

ParamEq::Band band(ParamEq::Type t, double f, double g, double q) {
    return ParamEq::Band{true, t, f, g, q};
}
}  // namespace

int main() {
    // 1) Bypass = flat 0 dB everywhere, and process() leaves audio untouched.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        eq.setBypass(true);
        CHECK(near(eq.magnitudeDb(100.0), 0.0, 1e-9));
        CHECK(near(eq.magnitudeDb(3000.0), 0.0, 1e-9));
        float x[4] = {0.1f, -0.2f, 0.3f, -0.4f};
        eq.process(x, 4);
        CHECK(near(x[0], 0.1, 1e-6) && near(x[3], -0.4, 1e-6));
    }

    // 2) All bands disabled = unity (curve flat, audio unchanged).
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        CHECK(near(eq.magnitudeDb(800.0), 0.0, 1e-9));
        CHECK(near(processGainDb(eq, 800.0), 0.0, 0.05));
    }

    // 3) Single peaking band +6 dB @1 kHz, Q=2: gain == dBgain at centre,
    //    ~0 dB far away, and process() matches magnitudeDb() at the centre.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        eq.setBand(0, band(ParamEq::Type::Peak, 1000.0, 6.0, 2.0));
        CHECK(near(eq.magnitudeDb(1000.0), 6.0, 0.05));
        CHECK(std::fabs(eq.magnitudeDb(100.0)) < 0.7);
        CHECK(std::fabs(eq.magnitudeDb(12000.0)) < 0.7);
        // The invariant: drawn curve == applied gain.
        CHECK(near(processGainDb(eq, 1000.0), eq.magnitudeDb(1000.0), 0.4));
    }

    // 4) Negative peak (cut) also matches.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        eq.setBand(0, band(ParamEq::Type::Peak, 1500.0, -8.0, 1.5));
        CHECK(near(eq.magnitudeDb(1500.0), -8.0, 0.05));
        CHECK(near(processGainDb(eq, 1500.0), eq.magnitudeDb(1500.0), 0.4));
    }

    // 5) Low-pass @1 kHz, Q=0.707: flat in passband, strong cut above, and
    //    process() attenuates a high tone to match the curve.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        eq.setBand(0, band(ParamEq::Type::LowPass, 1000.0, 0.0, 0.707));
        CHECK(std::fabs(eq.magnitudeDb(100.0)) < 0.6);
        CHECK(eq.magnitudeDb(12000.0) < -20.0);
        CHECK(processGainDb(eq, 8000.0) < -15.0);
        CHECK(near(processGainDb(eq, 8000.0), eq.magnitudeDb(8000.0), 0.6));
    }

    // 6) High-pass @300 Hz, Q=0.707: cuts lows, passes highs.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        eq.setBand(0, band(ParamEq::Type::HighPass, 300.0, 0.0, 0.707));
        CHECK(eq.magnitudeDb(50.0) < -10.0);
        CHECK(std::fabs(eq.magnitudeDb(5000.0)) < 0.6);
    }

    // 7) High-shelf +5 dB @4 kHz: lifts the top, leaves lows alone.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        eq.setBand(0, band(ParamEq::Type::HighShelf, 4000.0, 5.0, 0.707));
        CHECK(near(eq.magnitudeDb(15000.0), 5.0, 0.5));
        CHECK(std::fabs(eq.magnitudeDb(100.0)) < 0.5);
    }

    // 8) Makeup trim shifts the whole curve.
    {
        ParamEq eq;
        eq.setSampleRate(kFs);
        disableAll(eq);
        eq.setMakeupDb(3.0);
        CHECK(near(eq.magnitudeDb(1000.0), 3.0, 1e-6));
        CHECK(near(processGainDb(eq, 1000.0), 3.0, 0.05));
    }

    if (g_fail == 0)
        std::printf("test_parameq: ALL PASS\n");
    else
        std::printf("test_parameq: %d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
