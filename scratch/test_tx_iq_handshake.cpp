// Standalone unit test for TX-1 component 6: TxDspWorker EP2
// hand-off semaphore handshake + injectTxIq gate + reference
// quantization helper.
//
// Build (from repo root, in a vcvars64 shell):
//   cl /std:c++20 /EHsc /O2 /I src ^
//      scratch\test_tx_iq_handshake.cpp /Fe:scratch\test_tx_iq_handshake.exe
//   .\test_tx_iq_handshake.exe
//
// Tests cover the handshake mechanics in isolation:
//   * Two binary semaphores + shared 126-sample buffer behave
//     exactly like the verified reference's hsendIQSem +
//     hobbuffsRun[0] + outIQbufp pattern.
//   * Producer signals + blocks until consumer releases consumed.
//   * Consumer's non-blocking try_acquire returns false when
//     producer hasn't released dataReady (the underrun case).
//   * 64-→-126 accumulator behaviour: 2 producer pushes (128
//     samples) yields one consumer pull (126) + 2-sample carry.
//   * Reference-faithful symmetric round-to-nearest quantize
//     matches the formula in the verified reference (see
//     docs/architecture/tx1_ssb_design.md §5.7 for the cite).

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <semaphore>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("  FAIL: %s\n", what); }
    else     {              std::printf("  pass: %s\n", what); }
}

// Mirror of HL2Stream's reference-faithful quantize block in the
// EP2 packer.  Keep this in sync with hl2_stream.cpp:1898+ — when
// the inline impl changes, copy the change here too.
int quantizeI16Sym(float x) {
    const float scaled = x * 32767.0f;
    const float rnd = scaled >= 0.0f ? std::floor(scaled + 0.5f)
                                     : std::ceil (scaled - 0.5f);
    const float clamped = rnd < -32768.0f ? -32768.0f
                        : rnd >  32767.0f ?  32767.0f : rnd;
    return static_cast<int>(clamped);
}

// ── Test 1: quantize matches reference formula at endpoints + center
void testQuantize() {
    std::printf("[test] reference-faithful symmetric round-to-nearest\n");
    check(quantizeI16Sym( 0.0f)        ==      0, "0.0 -> 0");
    check(quantizeI16Sym( 1.0f)        ==  32767, "+1.0 saturates to +32767");
    check(quantizeI16Sym(-1.0f)        == -32767, "-1.0 -> -32767 (sym, not -32768)");
    check(quantizeI16Sym( 0.5f)        ==  16384, "+0.5 -> +16384 (rounded)");
    check(quantizeI16Sym(-0.5f)        == -16384, "-0.5 -> -16384 (sym rounded)");
    check(quantizeI16Sym( 1.5f)        ==  32767, "+1.5 clamped to +32767");
    check(quantizeI16Sym(-1.5f)        == -32768, "-1.5 clamped to -32768 (HW range)");
}

// ── Test 2: binary semaphore handshake — producer signals, consumer
//           reads, releases consumed; producer can refill.
void testHandshake() {
    std::printf("[test] two-semaphore handshake (reference pattern)\n");
    std::counting_semaphore<1> dataReady(0);
    std::counting_semaphore<1> consumed(1);  // start "consumed" =
                                             // buffer available
    std::vector<std::complex<float>> buf(126, {0.0f, 0.0f});
    std::atomic<int> producerFills{0};

    // Producer: fill the buffer with a unique marker per fill,
    // signal dataReady, wait for consumed.  Run a few iterations.
    std::thread producer([&]() {
        for (int round = 1; round <= 5; ++round) {
            consumed.acquire();  // wait for previous round consumed
            for (auto &s : buf) s = std::complex<float>(
                static_cast<float>(round), -static_cast<float>(round));
            producerFills.fetch_add(1);
            dataReady.release();
        }
    });

    // Consumer: try_acquire dataReady, read buf, release consumed.
    int consumed_rounds = 0;
    for (int i = 0; i < 5; ++i) {
        // Wait briefly for producer
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (dataReady.try_acquire()) {
            const float r = buf[0].real();
            check(r == static_cast<float>(i + 1),
                  "consumer reads correct marker for this round");
            consumed.release();
            ++consumed_rounds;
        }
    }
    producer.join();
    check(consumed_rounds == 5,  "consumer received all 5 producer fills");
    check(producerFills.load() == 5, "producer completed all 5 fills");
}

// ── Test 3: try_acquire returns false when dataReady not signalled
//           (the underrun case — fall through to zero-fill).
void testUnderrun() {
    std::printf("[test] non-blocking try_acquire returns false on no-data\n");
    std::counting_semaphore<1> dataReady(0);  // never released
    check(!dataReady.try_acquire(),
          "try_acquire on un-signalled semaphore returns false");
    check(!dataReady.try_acquire(),
          "second try also returns false (no spurious release)");
}

// ── Test 4: accumulator 64-→-126 behaviour.  Simulate 3 producer
//           pushes of 64 samples each (192 total); expect ONE 126-
//           sample drain + 66 samples carried over, then a second
//           drain after another 64 push (130 total).
void testAccumulator() {
    std::printf("[test] 64-sample producer → 126-sample consumer accumulator\n");
    constexpr int kBlock = 64;
    constexpr int kEp2   = 126;
    std::vector<std::complex<float>> accum;
    accum.reserve(2 * kBlock);

    auto pushBlock = [&](int marker) {
        for (int i = 0; i < kBlock; ++i)
            accum.emplace_back(static_cast<float>(marker),
                               static_cast<float>(i));
    };
    auto drainEp2 = [&]() -> std::vector<std::complex<float>> {
        std::vector<std::complex<float>> out(accum.begin(),
                                             accum.begin() + kEp2);
        accum.erase(accum.begin(), accum.begin() + kEp2);
        return out;
    };

    pushBlock(1);  pushBlock(2);  // 128 samples
    check(static_cast<int>(accum.size()) >= kEp2,
          "after 2 pushes (128) >= kEp2 (126)");
    auto drain1 = drainEp2();
    check(static_cast<int>(drain1.size()) == kEp2,
          "drain1 has exactly 126 samples");
    check(static_cast<int>(accum.size()) == 2,
          "2 samples carry over (128-126)");
    check(drain1[0].real() == 1.0f,
          "drain1 starts at marker 1 (first push)");
    check(drain1[64].real() == 2.0f,
          "drain1 transitions to marker 2 at sample 64");

    pushBlock(3);  // now 2 + 64 = 66, not enough yet
    check(static_cast<int>(accum.size()) < kEp2,
          "after carry+1 push (66) still < kEp2 — no drain");
    pushBlock(4);  // 66 + 64 = 130, ready
    check(static_cast<int>(accum.size()) >= kEp2,
          "after carry+2 pushes (130) ready to drain again");
    auto drain2 = drainEp2();
    check(static_cast<int>(drain2.size()) == kEp2,
          "drain2 has exactly 126 samples");
    check(static_cast<int>(accum.size()) == 4,
          "4 samples carry over (130-126)");
    check(drain2[0].real() == 2.0f,
          "drain2 starts at the 2-sample carryover from previous drain");
}

}  // namespace

int main() {
    std::printf("=== TX I/Q handshake unit tests ===\n");
    testQuantize();
    testHandshake();
    testUnderrun();
    testAccumulator();
    std::printf("=== %s (%d failures) ===\n",
                failures == 0 ? "ALL PASS" : "*** FAILURES ***", failures);
    return failures == 0 ? 0 : 1;
}
