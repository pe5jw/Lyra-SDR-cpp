// Lyra-cpp — #89 Voice keyer / RX recorder, Stage C: the capture core.
//
// The record-side twin of ClipRecorderPlayer (Stage A).  A thread-safe,
// Qt-free / DSP-free accumulator: a producer thread feeds 48 kHz mono
// samples while recording; the UI thread stop()s and takes the captured
// clip (→ ClipBank, Stage B3).  Two feed sources (design decisions 1-2):
//   * MIC — the raw AK4951 mic the reference records "pre-processed" (the
//     clip then runs the full TXA chain on playback, PS-safe); fed from the
//     Ep6 reader thread at the same mic-harvest point the injector taps.
//   * RX  — the receive audio (post-RXA "what you heard" by default); fed
//     from the audio thread (Stage C2).
// A start(Source) records which source is active; each feed accumulates
// ONLY when recording AND its source matches, so the two taps can both be
// installed and the active record decides which one fills the buffer.
//
// The idle path is lock-free: feed checks a recording atomic first and
// returns immediately when not recording (the common case), so it costs
// nothing on the producer thread until the operator hits REC.  A hard
// length cap bounds memory (a runaway record can't exhaust RAM).

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace lyra::tx {

class ClipRecorder {
public:
    enum class Source { Mic, Rx };

    ClipRecorder() = default;

    // Begin capturing from `src`.  Clears any prior buffer.  Idempotent-safe:
    // a start() while already recording restarts from empty.
    void start(Source src);

    // Producer-thread feeds.  Each is a no-op unless recording AND the active
    // source matches — and the lock-free recording gate is checked first so
    // the idle path takes no lock.
    //   MIC: interleaved {I = mic, Q} pairs (the Ep6 TxReadBufp shape); I is
    //        recorded, Q ignored.  n_pairs = number of (I,Q) pairs.
    void feedMicPairs(const double *iq_pairs, int n_pairs);
    //   RX: mono float samples (Stage C2).
    void feedRxMono(const float *mono, int n);

    // Stop + hand back the captured samples (moved out; the recorder empties).
    // Returns empty if nothing was captured.
    std::vector<float> stop();

    bool recording() const noexcept { return recording_.load(std::memory_order_acquire); }
    Source source()  const noexcept { return source_; }

    // Samples captured so far (approx; lock-free read of the size hint).
    int durationMs() const;

    static constexpr int sampleRate()  { return 48000; }
    static constexpr int kMaxSamples   = 48000 * 120;   // 120 s hard cap

private:
    mutable std::mutex mtx_;
    std::atomic<bool>  recording_{false};
    std::atomic<int>   size_{0};        // lock-free size hint for durationMs()
    Source             source_ = Source::Mic;
    std::vector<float> buf_;
};

} // namespace lyra::tx
