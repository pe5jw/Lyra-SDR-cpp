// Lyra-cpp — unit test for ClipRecorder (#89 Stage C capture core).
//   cmake --build build --target test_clip_recorder  &&  build/test_clip_recorder.exe

#include "tx/ClipRecorder.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lyra::tx;
using Source = ClipRecorder::Source;

static int g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }            \
        else         { std::printf("  ok  : %s\n", msg); }                      \
    } while (0)

int main() {
    std::printf("== ClipRecorder (voice-keyer / RX capture) ==\n");
    ClipRecorder rec;

    CHECK(!rec.recording(), "idle at construction");
    // Feeds while idle are ignored.
    double pairs[8] = {0.1, 0, 0.2, 0, 0.3, 0, 0.4, 0};
    rec.feedMicPairs(pairs, 4);
    CHECK(rec.stop().empty(), "feed while idle captures nothing");

    // ── mic capture: I recorded, Q ignored ──
    rec.start(Source::Mic);
    CHECK(rec.recording(), "recording after start(Mic)");
    CHECK(rec.source() == Source::Mic, "source = Mic");
    rec.feedMicPairs(pairs, 4);
    rec.feedRxMono(nullptr, 0);                 // wrong-source feed ignored
    { std::vector<float> rx = {9.0f, 9.0f}; rec.feedRxMono(rx.data(), 2); }
    rec.feedMicPairs(pairs, 4);                 // 4 more
    {
        auto clip = rec.stop();
        CHECK(clip.size() == 8, "8 mic samples (Q ignored, Rx feed ignored)");
        bool ok = clip.size() == 8
               && std::fabs(clip[0] - 0.1f) < 1e-6f && std::fabs(clip[1] - 0.2f) < 1e-6f
               && std::fabs(clip[4] - 0.1f) < 1e-6f;
        CHECK(ok, "mic I values recorded in order");
    }
    CHECK(!rec.recording(), "stopped after stop()");

    // ── RX capture ──
    rec.start(Source::Rx);
    CHECK(rec.source() == Source::Rx, "source = Rx");
    rec.feedMicPairs(pairs, 4);                 // wrong-source feed ignored
    { std::vector<float> rx = {0.5f, -0.5f, 0.25f}; rec.feedRxMono(rx.data(), 3); }
    {
        auto clip = rec.stop();
        CHECK(clip.size() == 3, "3 RX samples (mic feed ignored)");
        CHECK(std::fabs(clip[1] + 0.5f) < 1e-6f, "RX values recorded");
    }

    // ── RX stereo-dup (engine shape): L recorded, R ignored ──
    rec.start(Source::Rx);
    { double sd[6] = {0.7, 0.7, -0.3, -0.3, 0.2, 0.2}; rec.feedRxStereoDup(sd, 3); }
    rec.feedMicPairs(pairs, 4);                 // wrong-source feed ignored
    {
        auto clip = rec.stop();
        CHECK(clip.size() == 3, "3 RX stereo-dup samples (mic feed ignored)");
        bool ok = clip.size() == 3
               && std::fabs(clip[0] - 0.7f) < 1e-6f && std::fabs(clip[1] + 0.3f) < 1e-6f;
        CHECK(ok, "RX stereo-dup L values recorded");
    }

    // ── restart clears the prior buffer ──
    rec.start(Source::Mic);
    rec.feedMicPairs(pairs, 2);
    rec.start(Source::Mic);                     // restart mid-capture
    rec.feedMicPairs(pairs, 1);
    {
        auto clip = rec.stop();
        CHECK(clip.size() == 1, "restart clears the prior buffer");
    }

    // ── length cap ──
    rec.start(Source::Rx);
    std::vector<float> big(ClipRecorder::kMaxSamples + 5000, 0.0f);
    rec.feedRxMono(big.data(), static_cast<int>(big.size()));
    {
        auto clip = rec.stop();
        CHECK(static_cast<int>(clip.size()) == ClipRecorder::kMaxSamples,
              "capture capped at kMaxSamples");
    }

    std::printf(g_fail ? "\nRESULT: %d CHECK(S) FAILED\n" : "\nRESULT: ALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
