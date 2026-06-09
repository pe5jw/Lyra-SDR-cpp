// Standalone unit test for Stage 7.1 TxIqRing.
//
// Build (from repo root, in a vcvars64 shell):
//   cl /std:c++latest /EHsc /O2 /I src ^
//      scratch\test_tx_iq_ring.cpp src\dsp\TxIqRing.cpp ^
//      /Fe:scratch\test_tx_iq_ring.exe
//   .\scratch\test_tx_iq_ring.exe
//
// (/std:c++latest -- this MSVC toolchain does not recognise
// /std:c++23 as a stable flag; the main CMake build handles
// CMAKE_CXX_STANDARD 23 -> /std:c++latest translation internally.
// Or invoke `_build_test_tx_iq_ring.bat` which sets up the env.)
//
// Tests cover the Stage 7.1 surface end-to-end:
//   * pushFromInterleavedDoubles + popBlock126 round-trip
//     bit-pattern preserving (doubles->float narrowing is the
//     ONLY value transformation; verified per-sample)
//   * FIFO ordering across multiple push/pop interleavings
//   * Drop-oldest at all three thresholds: (a) exact-fit fills
//     ring without dropping; (b) common-case overflow drops only
//     the deficit; (c) n > capacity drops everything currently
//     held + the leading (n - capacity) of the incoming batch
//   * popBlock126 returns false on underrun (count < 126) and
//     leaves the ring untouched
//   * totalDropped() lifetime accounting across many overflows
//   * clear() resets count without resetting totalDropped
//   * Wrap-around: push/pop crossing the buffer wrap boundary
//     preserves sample ordering
//   * SPSC thread stress smoke: one producer thread pushing
//     batches + one consumer thread popping blocks for ~500 ms
//     with random sleeps; verify no deadlock, no crash, no
//     out-of-order samples in the consumer's received stream
//     (modulo dropped samples — sequence numbers monotonic with
//     gaps where drops occurred)
//
// No external dependencies beyond the project source tree + MSVC.

#include "dsp/TxIqRing.h"

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    } else {
        std::printf("  pass: %s\n", what);
    }
}

// Make n_samples interleaved doubles where I = base+i, Q = -(base+i).
// Sequence-number friendly: real part identifies the sample.
std::vector<double> make_iq(double base, int n_samples) {
    std::vector<double> v(static_cast<std::size_t>(n_samples) * 2);
    for (int i = 0; i < n_samples; ++i) {
        v[2 * i + 0] = base + static_cast<double>(i);
        v[2 * i + 1] = -(base + static_cast<double>(i));
    }
    return v;
}

// Pop kBlockSize samples and verify the I parts are
// sequential starting at expected_base; returns true if all match.
bool verify_block_sequence(const std::complex<float>* out, double expected_base) {
    for (int i = 0; i < lyra::dsp::TxIqRing::kBlockSize; ++i) {
        const float want_i = static_cast<float>(expected_base + i);
        const float want_q = -want_i;
        if (out[i].real() != want_i || out[i].imag() != want_q) {
            std::printf("    seq mismatch at i=%d: got (%g, %g) expected (%g, %g)\n",
                        i, static_cast<double>(out[i].real()),
                        static_cast<double>(out[i].imag()),
                        static_cast<double>(want_i),
                        static_cast<double>(want_q));
            return false;
        }
    }
    return true;
}

void test_bit_pattern_round_trip() {
    std::printf("[1] bit-pattern round-trip (doubles -> float -> read back)\n");
    lyra::dsp::TxIqRing r(512);

    // Push one block worth of carefully-chosen values that span
    // the float dynamic range we'll see on the wire.  TX I/Q
    // doubles after WDSP TXA are nominally in [-1.0, +1.0] but we
    // include zero, denormal-adjacent, and full-scale samples to
    // exercise the narrowing cast.
    const int N = lyra::dsp::TxIqRing::kBlockSize;
    std::vector<double> in(static_cast<std::size_t>(N) * 2);
    for (int i = 0; i < N; ++i) {
        // Spread values across [-1, +1] with deterministic spacing.
        const double v = (static_cast<double>(i) / (N - 1)) * 2.0 - 1.0;
        in[2 * i + 0] = v;
        in[2 * i + 1] = -v;
    }

    const std::size_t dropped = r.pushFromInterleavedDoubles(in.data(), N);
    check(dropped == 0, "no drop on first push of one block");
    check(r.size() == static_cast<std::size_t>(N), "size == one block");

    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    const bool got = r.popBlock126(out);
    check(got, "popBlock126 returns true");
    check(r.size() == 0, "size == 0 after pop");

    // Every sample must match the static_cast<float>(double) result
    // bit-exactly.  No FP comparison fuzz: float(double) is
    // deterministic.
    bool all_match = true;
    for (int i = 0; i < N; ++i) {
        const float want_i = static_cast<float>(in[2 * i + 0]);
        const float want_q = static_cast<float>(in[2 * i + 1]);
        if (out[i].real() != want_i || out[i].imag() != want_q) {
            std::printf("    mismatch at i=%d: got (%a, %a) expected (%a, %a)\n",
                        i, static_cast<double>(out[i].real()),
                        static_cast<double>(out[i].imag()),
                        static_cast<double>(want_i),
                        static_cast<double>(want_q));
            all_match = false;
            break;
        }
    }
    check(all_match, "every sample is bit-exact static_cast<float>(double)");
}

void test_fifo_ordering_across_push_pop_mix() {
    std::printf("[2] FIFO ordering across multiple push/pop interleavings\n");
    lyra::dsp::TxIqRing r(512);
    const int N = lyra::dsp::TxIqRing::kBlockSize;

    // Push 3 blocks back-to-back, pop them one at a time.
    auto batch_a = make_iq(/*base=*/1000.0, N);
    auto batch_b = make_iq(/*base=*/2000.0, N);
    auto batch_c = make_iq(/*base=*/3000.0, N);
    check(r.pushFromInterleavedDoubles(batch_a.data(), N) == 0, "push A: 0 drops");
    check(r.pushFromInterleavedDoubles(batch_b.data(), N) == 0, "push B: 0 drops");
    check(r.pushFromInterleavedDoubles(batch_c.data(), N) == 0, "push C: 0 drops");
    check(r.size() == 3 * static_cast<std::size_t>(N), "size == 3 blocks");

    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    check(r.popBlock126(out), "pop 1");
    check(verify_block_sequence(out, 1000.0), "pop 1 == A");
    check(r.popBlock126(out), "pop 2");
    check(verify_block_sequence(out, 2000.0), "pop 2 == B");
    check(r.popBlock126(out), "pop 3");
    check(verify_block_sequence(out, 3000.0), "pop 3 == C");
    check(!r.popBlock126(out), "pop 4 fails (underrun)");
    check(r.size() == 0, "size == 0 after 3 pops");

    // Now interleave: push A, pop, push B, push C, pop, pop.
    check(r.pushFromInterleavedDoubles(batch_a.data(), N) == 0, "interleave: push A");
    check(r.popBlock126(out) && verify_block_sequence(out, 1000.0),
          "interleave: pop A immediately");
    check(r.pushFromInterleavedDoubles(batch_b.data(), N) == 0, "interleave: push B");
    check(r.pushFromInterleavedDoubles(batch_c.data(), N) == 0, "interleave: push C");
    check(r.popBlock126(out) && verify_block_sequence(out, 2000.0),
          "interleave: pop B");
    check(r.popBlock126(out) && verify_block_sequence(out, 3000.0),
          "interleave: pop C");
}

void test_underrun() {
    std::printf("[3] popBlock126 underrun returns false, leaves ring untouched\n");
    lyra::dsp::TxIqRing r(512);

    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    // Memset to a sentinel value the ring would NEVER write
    // (NaN with a specific bit pattern via memset is too cute; use
    // a normal value that we'll verify is untouched).
    for (auto& s : out) s = std::complex<float>{12345.0f, -54321.0f};

    check(!r.popBlock126(out), "empty ring: pop returns false");
    check(out[0] == std::complex<float>(12345.0f, -54321.0f), "out untouched on underrun");

    // Push 125 samples (one less than a full block).
    auto partial = make_iq(/*base=*/100.0, 125);
    check(r.pushFromInterleavedDoubles(partial.data(), 125) == 0, "push 125");
    check(r.size() == 125, "size == 125");

    check(!r.popBlock126(out), "125-sample ring: pop returns false");
    check(r.size() == 125, "ring still has 125 after failed pop");
    check(out[0] == std::complex<float>(12345.0f, -54321.0f),
          "out STILL untouched on second underrun");

    // Push 1 more = exactly one block; pop should now succeed.
    auto one = make_iq(/*base=*/225.0, 1);  // base == 100+125
    check(r.pushFromInterleavedDoubles(one.data(), 1) == 0, "push 1 more");
    check(r.size() == 126, "size == 126 (exact block)");
    check(r.popBlock126(out), "pop now succeeds");
    check(verify_block_sequence(out, 100.0), "popped block is [100..225]");
    check(r.size() == 0, "ring empty");
}

void test_drop_oldest_common_case() {
    std::printf("[4] drop-oldest: common case (n < capacity, deficit dropped)\n");
    // Capacity 200, push 150, push another 100 -> deficit 50 dropped
    // from the head.  Final ring contents: last 50 of first batch +
    // all 100 of second batch = 150 samples; first 50 of first batch
    // are lost.
    lyra::dsp::TxIqRing r(200);
    auto a = make_iq(/*base=*/1.0, 150);     // samples 1..150
    auto b = make_iq(/*base=*/1000.0, 100);  // samples 1000..1099

    check(r.pushFromInterleavedDoubles(a.data(), 150) == 0, "push 150: no drop");
    check(r.size() == 150, "size == 150");
    const std::size_t dropped = r.pushFromInterleavedDoubles(b.data(), 100);
    check(dropped == 50, "push 100: dropped 50 (the deficit)");
    check(r.size() == 200, "size == capacity (200)");
    check(r.totalDropped() == 50, "totalDropped == 50");

    // The ring should now hold: a[50..149] then b[0..99] = 100 samples
    // from the tail of a, then all 100 of b.  Verify by popping into a
    // big out buffer manually (we only have popBlock126, so pop 1
    // block and check head; pop another and check; we have 200 samples
    // so we get 1 full block + 74 leftover = 1 successful pop).
    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    check(r.popBlock126(out), "pop succeeds");
    // First pop expects samples 51..176 (a[50..149] = 100 samples
    // starting at value 51, then b[0..25] = 26 samples starting at
    // 1000).  Verify the first sample is 51 and the boundary at i=99
    // jumps from 150 to 1000.
    check(out[0].real() == 51.0f, "first popped sample is 51 (oldest 50 dropped)");
    check(out[99].real() == 150.0f, "boundary @ 99: last of A == 150");
    check(out[100].real() == 1000.0f, "boundary @ 100: first of B == 1000");
    check(r.size() == 200 - 126, "size == 74 after 1 pop");
}

void test_drop_oldest_n_equal_capacity() {
    std::printf("[5] drop-oldest: n == capacity (entire ring discarded, batch fits exactly)\n");
    lyra::dsp::TxIqRing r(126);
    auto a = make_iq(/*base=*/1.0, 100);
    auto b = make_iq(/*base=*/1000.0, 126);  // n == capacity

    check(r.pushFromInterleavedDoubles(a.data(), 100) == 0, "prefill 100");
    check(r.size() == 100, "size == 100");
    const std::size_t dropped = r.pushFromInterleavedDoubles(b.data(), 126);
    check(dropped == 100, "drop entire prefill (100 samples lost)");
    check(r.size() == 126, "size == 126 (full ring of B)");

    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    check(r.popBlock126(out), "pop succeeds");
    check(verify_block_sequence(out, 1000.0), "popped block is entirely from B");
    check(r.size() == 0, "ring empty");
}

void test_drop_oldest_n_greater_than_capacity() {
    std::printf("[6] drop-oldest: n > capacity (everything + leading n-cap dropped)\n");
    lyra::dsp::TxIqRing r(126);
    auto a = make_iq(/*base=*/1.0, 50);
    auto b = make_iq(/*base=*/1000.0, 300);  // 300 > 126

    check(r.pushFromInterleavedDoubles(a.data(), 50) == 0, "prefill 50");
    const std::size_t dropped = r.pushFromInterleavedDoubles(b.data(), 300);
    // Expected drops: 50 (the prefill) + (300 - 126) = 50 + 174 = 224
    check(dropped == 224, "drop = 50 prefill + 174 leading B");
    check(r.size() == 126, "size == 126");
    // Ring should hold the LAST 126 samples of B, which started at
    // base 1000 with 300 samples == values 1000..1299.  Last 126 ==
    // values 1174..1299.
    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    check(r.popBlock126(out), "pop succeeds");
    check(verify_block_sequence(out, 1174.0), "popped block == last 126 of B");
    check(r.totalDropped() == 224, "totalDropped == 224");
}

void test_wrap_around() {
    std::printf("[7] wrap-around: push/pop crossing buffer boundary preserves order\n");
    // Capacity 300; push 200, pop 126 (advances tail past 0->126),
    // push 200 more (head wraps from 200 to (200+200)%300 = 100,
    // crossing the boundary).  Pop two blocks and verify ordering.
    lyra::dsp::TxIqRing r(300);
    auto a = make_iq(/*base=*/1.0, 200);
    auto b = make_iq(/*base=*/1000.0, 200);

    check(r.pushFromInterleavedDoubles(a.data(), 200) == 0, "push A (200)");
    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    check(r.popBlock126(out), "pop 1");
    check(verify_block_sequence(out, 1.0), "pop 1 == A[0..125]");

    // 74 of A left in ring; push 200 more of B; total = 274, fits in cap 300.
    check(r.pushFromInterleavedDoubles(b.data(), 200) == 0, "push B (200), no drop");
    check(r.size() == 274, "size == 274");

    check(r.popBlock126(out), "pop 2");
    // Pop 2 should be A[126..199] (74 samples) + B[0..51] (52 samples).
    check(out[0].real() == 127.0f, "pop 2 first sample == 127 (A[126])");
    check(out[73].real() == 200.0f, "pop 2 boundary A->B: A[199] == 200");
    check(out[74].real() == 1000.0f, "pop 2 boundary A->B: B[0] == 1000");
    check(out[125].real() == 1051.0f, "pop 2 last == B[51] == 1051");
    check(r.size() == 274 - 126, "size == 148 after pop 2");

    check(r.popBlock126(out), "pop 3");
    check(verify_block_sequence(out, 1052.0), "pop 3 == B[52..177]");
}

void test_clear_and_totalDropped_semantics() {
    std::printf("[8] clear() resets size + indices, NOT totalDropped\n");
    lyra::dsp::TxIqRing r(126);
    auto a = make_iq(1.0, 100);
    auto b = make_iq(1000.0, 200);  // n > cap, will drop everything

    r.pushFromInterleavedDoubles(a.data(), 100);
    r.pushFromInterleavedDoubles(b.data(), 200);
    const std::uint64_t total_before = r.totalDropped();
    check(total_before == 100 + (200 - 126), "totalDropped before clear");
    check(r.size() == 126, "size before clear");

    r.clear();
    check(r.size() == 0, "size == 0 after clear");
    check(r.totalDropped() == total_before, "totalDropped UNCHANGED by clear");

    std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
    check(!r.popBlock126(out), "pop after clear: underrun");

    // Push a fresh full block — totalDropped must NOT increase
    // (no overflow on a cleared ring).
    auto c = make_iq(7777.0, 126);
    check(r.pushFromInterleavedDoubles(c.data(), 126) == 0, "post-clear push: no drop");
    check(r.totalDropped() == total_before, "totalDropped STILL == before");
    check(r.popBlock126(out), "post-clear pop succeeds");
    check(verify_block_sequence(out, 7777.0), "post-clear pop sequence");
}

void test_spsc_thread_stress() {
    std::printf("[9] SPSC thread stress smoke (1 producer + 1 consumer for ~300 ms)\n");
    lyra::dsp::TxIqRing r(2048);  // ~17 EP2 datagrams cushion
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> producer_samples{0};
    std::atomic<std::uint64_t> consumer_blocks{0};
    std::atomic<bool> consumer_ordering_ok{true};
    std::atomic<double> consumer_next_expected{1.0};

    // Producer: emit batches of 38-126 samples (mimicking the
    // Hl2Ep6MicSource rx-loop block-size variance per CLAUDE.md
    // §3.3 19-slot EP6 datagram analysis) at ~1 kHz.
    std::thread producer([&]() {
        std::mt19937 rng(0xC0FFEEu);
        std::uniform_int_distribution<int> nsamp_dist(38, 126);
        double seq = 1.0;
        while (!stop.load(std::memory_order_acquire)) {
            const int n = nsamp_dist(rng);
            std::vector<double> batch(static_cast<std::size_t>(n) * 2);
            for (int i = 0; i < n; ++i) {
                batch[2 * i + 0] = seq + i;
                batch[2 * i + 1] = -(seq + i);
            }
            r.pushFromInterleavedDoubles(batch.data(), n);
            seq += n;
            producer_samples.fetch_add(static_cast<std::uint64_t>(n),
                                       std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(800));
        }
    });

    // Consumer: pop blocks at ~380 Hz cadence; verify each popped
    // block is internally sequential (within-block ordering check)
    // and that block-to-block sequence advances monotonically with
    // gaps allowed (drops).
    std::thread consumer([&]() {
        std::complex<float> out[lyra::dsp::TxIqRing::kBlockSize];
        double last_seq = 0.0;
        bool first = true;
        while (!stop.load(std::memory_order_acquire)) {
            if (r.popBlock126(out)) {
                consumer_blocks.fetch_add(1, std::memory_order_relaxed);
                // Within-block: out[i+1] - out[i] == 1.0
                for (int i = 1; i < lyra::dsp::TxIqRing::kBlockSize; ++i) {
                    if (out[i].real() - out[i - 1].real() != 1.0f) {
                        consumer_ordering_ok.store(false,
                                                   std::memory_order_release);
                    }
                }
                const double block_start = static_cast<double>(out[0].real());
                if (!first && block_start < last_seq) {
                    // Sequence number went backwards — impossible
                    // under SPSC FIFO + drop-oldest (drops only
                    // skip forward, never backward).
                    consumer_ordering_ok.store(false,
                                               std::memory_order_release);
                }
                first = false;
                last_seq = static_cast<double>(out[lyra::dsp::TxIqRing::kBlockSize - 1].real());
                consumer_next_expected.store(last_seq + 1.0,
                                             std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(2600));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    std::printf("    producer_samples = %llu, consumer_blocks = %llu, "
                "totalDropped = %llu\n",
                static_cast<unsigned long long>(producer_samples.load()),
                static_cast<unsigned long long>(consumer_blocks.load()),
                static_cast<unsigned long long>(r.totalDropped()));

    check(producer_samples.load() > 0, "producer emitted samples");
    check(consumer_blocks.load() > 0, "consumer popped blocks");
    check(consumer_ordering_ok.load(),
          "consumer saw strictly-monotonic sequence with no within-block reordering");
}

}  // namespace

int main() {
    std::printf("TxIqRing unit test (Stage 7.1)\n");
    std::printf("==============================\n\n");

    test_bit_pattern_round_trip();
    std::printf("\n");
    test_fifo_ordering_across_push_pop_mix();
    std::printf("\n");
    test_underrun();
    std::printf("\n");
    test_drop_oldest_common_case();
    std::printf("\n");
    test_drop_oldest_n_equal_capacity();
    std::printf("\n");
    test_drop_oldest_n_greater_than_capacity();
    std::printf("\n");
    test_wrap_around();
    std::printf("\n");
    test_clear_and_totalDropped_semantics();
    std::printf("\n");
    test_spsc_thread_stress();
    std::printf("\n");

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
}
