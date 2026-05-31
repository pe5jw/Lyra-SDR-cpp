// Standalone unit test for TX-1 component 5a: MoxEdgeFade.
//
// Build (from repo root, in a vcvars64 shell):
//   cl /std:c++20 /EHsc /O2 /I src ^
//      scratch\test_mox_edge_fade.cpp src\mox_edge_fade.cpp
//   .\test_mox_edge_fade.exe
//
// Tests cover:
//   * Initial state = Idle (coef = 0)
//   * Fade-in: coef rises 0 → 1 over fadeInMs (cos² shape, midpoint = 0.5)
//   * Fade-out: coef falls 1 → 0 over fadeOutMs (cos² shape)
//   * Steady state On between fades
//   * Mid-fade reversal — coef value preserved across state flip
//   * Operator-tunable durations clamp to bounds
//   * Idempotent notifyMoxState (no-op on no-edge)

#include "mox_edge_fade.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what, double got = 0.0, double want = 0.0) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s  (got %.6f, expected %.6f)\n",
                    what, got, want);
    } else {
        std::printf("  pass: %s\n", what);
    }
}

bool approx(float a, float b, float tol) {
    return std::fabs(a - b) <= tol;
}

void testInitialState() {
    std::printf("[test] initial state\n");
    lyra::dsp::MoxEdgeFade f;
    check(f.isIdle(),                  "starts Idle");
    check(!f.isOn(),                   "starts !On");
    check(approx(f.advance(), 0.0f, 0.0f), "advance() = 0 at Idle",
          f.advance(), 0.0f);
    // Defaults
    check(f.fadeInMs() == lyra::dsp::MoxEdgeFade::kDefaultFadeInMs,
          "fadeInMs default = 50",  f.fadeInMs(),
          lyra::dsp::MoxEdgeFade::kDefaultFadeInMs);
    check(f.fadeOutMs() == lyra::dsp::MoxEdgeFade::kDefaultFadeOutMs,
          "fadeOutMs default = 13", f.fadeOutMs(),
          lyra::dsp::MoxEdgeFade::kDefaultFadeOutMs);
}

void testFadeInShape() {
    std::printf("[test] fade-in cos² shape (50 ms @ 48 kHz = 2400 samples)\n");
    lyra::dsp::MoxEdgeFade f;
    f.notifyMoxState(true);  // rising edge → start FadingIn
    const int N = 2400;
    // Sample boundaries: coef[0]=0, coef[N/2]≈0.5, coef[N]=1
    const float c0    = f.advance();           // phase 0
    float cHalf = 0.0f;
    for (int i = 1; i < N / 2; ++i) f.advance();
    cHalf = f.advance();                       // phase N/2
    for (int i = N / 2 + 1; i < N; ++i) f.advance();
    const float cEnd  = f.advance();           // phase N → snap to 1
    check(approx(c0,    0.0f, 1e-6f),  "coef at phase 0 = 0",     c0,    0.0f);
    check(approx(cHalf, 0.5f, 1e-3f),  "coef at phase N/2 ≈ 0.5", cHalf, 0.5f);
    check(approx(cEnd,  1.0f, 1e-6f),  "coef at phase N = 1",     cEnd,  1.0f);
    check(f.isOn(),                    "state = On after fade complete");
    // Subsequent calls stay at 1.0
    check(approx(f.advance(), 1.0f, 0.0f), "steady-state On = 1", f.advance(), 1.0f);
}

void testFadeOutShape() {
    std::printf("[test] fade-out cos² shape (13 ms @ 48 kHz = 624 samples)\n");
    lyra::dsp::MoxEdgeFade f;
    // Jump to On state by completing a fade-in.
    f.notifyMoxState(true);
    for (int i = 0; i < 2401; ++i) f.advance();
    if (!f.isOn()) { ++failures; std::printf("  FAIL: setup didn't reach On\n"); return; }
    f.notifyMoxState(false);  // falling edge → start FadingOut
    const int N = 624;
    const float c0    = f.advance();           // phase 0
    float cHalf = 0.0f;
    for (int i = 1; i < N / 2; ++i) f.advance();
    cHalf = f.advance();                       // phase N/2
    for (int i = N / 2 + 1; i < N; ++i) f.advance();
    const float cEnd  = f.advance();           // phase N → snap to 0
    check(approx(c0,    1.0f, 1e-6f),  "coef at phase 0 = 1",     c0,    1.0f);
    check(approx(cHalf, 0.5f, 1e-2f),  "coef at phase N/2 ≈ 0.5", cHalf, 0.5f);
    check(approx(cEnd,  0.0f, 1e-6f),  "coef at phase N = 0",     cEnd,  0.0f);
    check(f.isIdle(),                  "state = Idle after fade complete");
}

void testMidFadeReversal() {
    std::printf("[test] mid-fade reversal continuity (coef preserved)\n");
    lyra::dsp::MoxEdgeFade f;
    f.notifyMoxState(true);  // start fade-in
    // Advance partway — say 1200 samples (halfway through 50 ms fade-in).
    for (int i = 0; i < 1200; ++i) f.advance();
    const float coefBefore = f.advance();  // coef at phase ≈ 1200 ≈ 0.5
    f.notifyMoxState(false);               // reverse — start fade-out at
                                           // complementary phase
    const float coefAfter = f.advance();
    // The coefficients should be CONTINUOUS — same value at the reversal
    // boundary.  Tolerance loose because cosineFadeIn(1201) and
    // cosineFadeOut(remapped_phase) involve int truncation.
    check(approx(coefBefore, coefAfter, 5e-3f),
          "coef continuous across mid-fade reversal",
          coefAfter, coefBefore);
}

void testReverseAfterPartialFadeOut() {
    std::printf("[test] re-key during fade-out (rising edge from FadingOut)\n");
    lyra::dsp::MoxEdgeFade f;
    // Get to On.
    f.notifyMoxState(true);
    for (int i = 0; i < 2401; ++i) f.advance();
    // Start fade-out, advance partway.
    f.notifyMoxState(false);
    for (int i = 0; i < 300; ++i) f.advance();  // ~half of 624
    const float coefBefore = f.advance();
    // Re-key — should pivot back to FadingIn.
    f.notifyMoxState(true);
    const float coefAfter = f.advance();
    check(approx(coefBefore, coefAfter, 5e-3f),
          "coef continuous across re-key (FadingOut → FadingIn)",
          coefAfter, coefBefore);
}

void testClamping() {
    std::printf("[test] operator-tunable durations clamp to bounds\n");
    lyra::dsp::MoxEdgeFade f;
    f.setFadeInMs(-100);
    check(f.fadeInMs() == lyra::dsp::MoxEdgeFade::kMinFadeMs,
          "negative ms clamps to kMinFadeMs",
          f.fadeInMs(), lyra::dsp::MoxEdgeFade::kMinFadeMs);
    f.setFadeInMs(99999);
    check(f.fadeInMs() == lyra::dsp::MoxEdgeFade::kMaxFadeMs,
          "huge ms clamps to kMaxFadeMs",
          f.fadeInMs(), lyra::dsp::MoxEdgeFade::kMaxFadeMs);
    f.setFadeOutMs(25);
    check(f.fadeOutMs() == 25,
          "in-range ms passes through unchanged",
          f.fadeOutMs(), 25);
}

void testNoEdgeNoOp() {
    std::printf("[test] notifyMoxState idempotent on no-edge\n");
    lyra::dsp::MoxEdgeFade f;
    f.notifyMoxState(false);  // already Idle, prevMox_=false
    check(f.isIdle(), "still Idle after redundant false notify");
    f.notifyMoxState(true);
    for (int i = 0; i < 2401; ++i) f.advance();
    if (!f.isOn()) { ++failures; std::printf("  FAIL: setup\n"); return; }
    f.notifyMoxState(true);
    check(f.isOn(), "still On after redundant true notify");
    check(approx(f.advance(), 1.0f, 0.0f), "coef still 1 (no fade-in restart)",
          f.advance(), 1.0f);
}

} // namespace

int main() {
    std::printf("=== MoxEdgeFade unit tests ===\n");
    testInitialState();
    testFadeInShape();
    testFadeOutShape();
    testMidFadeReversal();
    testReverseAfterPartialFadeOut();
    testClamping();
    testNoEdgeNoOp();
    std::printf("=== %s (%d failures) ===\n",
                failures == 0 ? "ALL PASS" : "*** FAILURES ***", failures);
    return failures == 0 ? 0 : 1;
}
