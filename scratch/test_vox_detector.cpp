// Standalone unit test for lyra::tx::VoxDetector (#91).
//
// Build (from repo root, in an MSVC env):
//   cl /std:c++20 /EHsc /I src scratch\test_vox_detector.cpp ^
//      src\tx\VoxDetector.cpp /Fe:build\test_vox.exe && build\test_vox.exe
//
// Pure class, no Qt/WDSP deps — links only VoxDetector.cpp.

#include "tx/VoxDetector.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using lyra::tx::VoxDetector;

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_fail; }          \
        else         { std::printf("ok  : %s\n", msg); }                    \
    } while (0)

// dBFS → linear RMS.
static double db(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

int main() {
    const double MIC_LOUD  = db(-20.0);   // well above the -35 threshold
    const double MIC_QUIET = db(-60.0);   // well below
    const double RX_LOUD   = db(-30.0);   // above the -45 anti-VOX level
    const double RX_QUIET  = db(-60.0);   // below

    VoxDetector::Params p;                 // defaults: -35 / -45 / 10 ms / 300 ms / anti ON
    p.openMs = 10; p.hangMs = 300;

    // 1. Silence never keys.
    {
        VoxDetector v; v.setParams(p);
        bool k = false;
        for (int i = 0; i < 100; ++i) k = v.tick(MIC_QUIET, RX_QUIET, 5.0);
        CHECK(!k, "silence never keys");
    }

    // 2. Open-delay: loud mic for < openMs does NOT open (1 tick of 5 ms).
    {
        VoxDetector v; v.setParams(p);
        bool k = v.tick(MIC_LOUD, RX_QUIET, 5.0);   // 5 ms < 10 ms openMs
        CHECK(!k, "open-delay: single 5 ms tick does not open");
        k = v.tick(MIC_LOUD, RX_QUIET, 5.0);        // now 10 ms total
        CHECK(k, "open-delay: opens once openMs is reached");
    }

    // 3. Hang: after opening, mic drops → holds for hangMs then closes.
    {
        VoxDetector v; v.setParams(p);
        v.tick(MIC_LOUD, RX_QUIET, 10.0);           // open
        CHECK(v.keyed(), "keyed after open");
        bool stillKeyedAt250 = true;
        for (int t = 0; t < 50; ++t)                // 50 * 5 ms = 250 ms < 300
            stillKeyedAt250 = v.tick(MIC_QUIET, RX_QUIET, 5.0);
        CHECK(stillKeyedAt250, "held through the hang (250 ms < 300 ms)");
        bool k = true;
        for (int t = 0; t < 20; ++t)                // +100 ms → past 300 ms
            k = v.tick(MIC_QUIET, RX_QUIET, 5.0);
        CHECK(!k, "closes after the hang expires");
    }

    // 4a. Anti-VOX suppresses opening when RX audio is loud.
    {
        VoxDetector v; v.setParams(p);
        bool k = false;
        for (int i = 0; i < 100; ++i) k = v.tick(MIC_LOUD, RX_LOUD, 5.0);
        CHECK(!k, "anti-VOX blocks opening while RX audio is loud");
    }
    // 4b. …but opens fine when RX is quiet.
    {
        VoxDetector v; v.setParams(p);
        v.tick(MIC_LOUD, RX_QUIET, 10.0);
        CHECK(v.keyed(), "opens with mic loud + RX quiet");
    }
    // 4c. Anti-VOX OFF → RX loud no longer blocks.
    {
        VoxDetector::Params np = p; np.antiVoxOn = false;
        VoxDetector v; v.setParams(np);
        v.tick(MIC_LOUD, RX_LOUD, 10.0);
        CHECK(v.keyed(), "anti-VOX disabled: opens even with RX loud");
    }

    // 5. Anti-VOX does NOT drop an already-open key (RX goes loud mid-TX).
    {
        VoxDetector v; v.setParams(p);
        v.tick(MIC_LOUD, RX_QUIET, 10.0);           // open (RX quiet)
        CHECK(v.keyed(), "open before RX rises");
        bool k = v.tick(MIC_LOUD, RX_LOUD, 5.0);    // RX now loud, mic still loud
        CHECK(k, "already-open key is not dropped by anti-VOX");
    }

    // 6. reset() forces closed + clears accumulators.
    {
        VoxDetector v; v.setParams(p);
        v.tick(MIC_LOUD, RX_QUIET, 10.0);
        CHECK(v.keyed(), "keyed before reset");
        v.reset();
        CHECK(!v.keyed(), "reset closes the gate");
        bool k = v.tick(MIC_LOUD, RX_QUIET, 5.0);   // must re-earn the open-delay
        CHECK(!k, "reset cleared the open accumulator");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "TEST FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
