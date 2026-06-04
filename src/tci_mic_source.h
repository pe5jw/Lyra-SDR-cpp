// Lyra-cpp — Task #33: TciMicSource.
//
// Sibling of Hl2Ep6MicSource at the OPERATOR-FACING interface, but
// architecturally a different shape because TCI clients deliver
// TX audio in BURSTS (MSHV: 2400-sample mono blocks every 50 ms)
// while codec mic delivers a few samples per EP6 datagram (~5 ms).
// Single-stage feed into TxDspWorker's small SPSC ring breaks
// catastrophically on the bursty-producer side (~80 % of FT8 modem
// audio dropped at the front door — operator-bench 2026-06-01).
//
// Architecture (Thetis-faithful, two-stage — operator-locked
// 2026-06-01 with PureSignal + full-duplex forward-compat in mind):
//
//   TCI client (MSHV / JTDX / ...)
//        │   TX_AUDIO_STREAM binary frame
//        │   2400 samples / 50 ms
//        ▼
//   onBinaryMessage decode  (Qt main thread)
//        │   submitFromTci(samples, n)
//        ▼
//   ┌─────────────────────────────────────────┐
//   │ TciMicSource: INBOUND QUEUE             │
//   │   kInboundCapacity = 96 000 samples     │
//   │   (≈ 2 sec @ 48 kHz — matches the       │
//   │   working reference's MAX_TX_AUDIO_      │
//   │   QUEUE_COMPLEX_SAMPLES at              │
//   │   TCIServer.cs:764)                     │
//   │                                         │
//   │   Drop-OLDEST on overrun (reference     │
//   │   parity — TCIServer.cs:5689-5702)      │
//   └────────┬────────────────────────────────┘
//            │  drain @ 10 ms cadence
//            │  pumps up to 480 samples per fire
//            │  (steady-state ≈ MSHV producer rate)
//            ▼
//   TxDspWorker::submitMicSamples(MicSource::Tci, …)
//        │
//        ▼
//   TxRing (small final-stage SPSC, ~10 ms, original sizing)
//        │
//        ▼
//   TX DSP worker thread → WDSP TXA → EP2 → HL2
//
// The inbound queue absorbs producer-side burst jitter; the final-
// stage TxRing stays small because it only sees the drain timer's
// rate-paced 480-sample chunks, which the worker drains at its
// natural 48 kHz / 64-sample-block cadence.  Steady-state ring
// depth ≈ 0; the queue absorbs everything upstream.
//
// THREADING (single-producer / single-consumer all on Qt main):
//
//   * TciServer's onBinaryMessage runs on Qt main → calls
//     submitFromTci → push to inbound queue.
//   * Drain QTimer fires on Qt main → pops from inbound queue →
//     calls TxDspWorker::submitMicSamples (which pushes into
//     TxRing; TxRing's single-producer contract holds because
//     Qt main is the sole producer for the TCI path AND the
//     codec-mic path is on a different producer thread but
//     gated by activeMicSource — only ONE source admits to the
//     ring at any instant).
//   * TxDspWorker's worker thread is the sole TxRing consumer.
//
// No mutex needed on inbound queue — single-threaded access from
// Qt main.
//
// FORWARD-COMPAT (operator-flagged 2026-06-01: "this is going to
// bite when full-duplex and PureSignal land"):
//
//   * PureSignal (v0.3) — calcc thread needs tight time-alignment
//     between TX I/Q (sip1 ring) and feedback I/Q (DDC2/3).  The
//     two-stage queue here decouples producer-burst jitter from
//     the wire-side cadence so the sip1 / feedback alignment math
//     isn't fighting upstream burst-distortion at the TX-audio
//     front door.
//   * Full duplex — RX2 + TX simultaneous operation requires the
//     TX path to NOT be a producer-rate bottleneck on the wire.
//     Inbound queue absorbs MSHV's burst pattern; final-stage
//     ring stays small and predictable for the worker.
//
// First-light client: MSHV (FT8 / FT4 / MSK144 / Q65 / ...).  Per
// docs/refs/mshv_tci/README.md, MSHV streams FLOAT32 mono @ 48 kHz
// in 2048-sample blocks with a 50 ms TX-buffering hint.

#pragma once

#include "tx_dsp_worker.h"

#include <QObject>

#include <deque>

class QTimer;

namespace lyra::dsp {

class TciMicSource : public QObject {
    Q_OBJECT
public:
    // Construct with a back-pointer to the TxDspWorker we feed.
    // TxDspWorker must outlive this object (caller arranges
    // teardown order: TciMicSource destroyed BEFORE TxDspWorker,
    // mirroring the existing Hl2Ep6MicSource ordering in main.cpp).
    // Drain QTimer starts in the ctor; stops in the dtor — no
    // separate enable() / disable() needed.
    explicit TciMicSource(TxDspWorker *worker, QObject *parent = nullptr);
    ~TciMicSource() override;

    TciMicSource(const TciMicSource &)            = delete;
    TciMicSource &operator=(const TciMicSource &) = delete;

    // Push N float32 mono samples @ 48 kHz into the INBOUND QUEUE.
    // Returns immediately if the worker isn't connected.  Caller
    // (TciServer's binary frame handler) is responsible for any
    // sample-rate / format / channel-count conversion BEFORE
    // calling here — this API takes the canonical TX-ring format
    // (FLOAT32 mono 48 kHz).  Drop-OLDEST on inbound-queue overrun
    // (working reference parity — TCIServer.cs:5689-5702).
    //
    // The actual push into TxDspWorker's ring happens on the drain
    // timer firing, NOT here — that decouples producer-burst
    // jitter from the worker's wire-side cadence.  Per
    // class-comment §"Architecture" diagram above.
    void submitFromTci(const float *samples, int n) noexcept;

    // Diagnostics — total samples submitted (regardless of whether
    // they were actually admitted to the queue / ring).  Operator
    // can compare against droppedSamples() + TxDspWorker overrun
    // to localize losses.
    long long submittedSamples() const noexcept {
        return submittedSamples_.load(std::memory_order_relaxed);
    }
    // Samples dropped at the inbound-queue front door (drop-oldest
    // on overrun).  Should stay near 0 in healthy operation;
    // sustained non-zero = drain timer falling behind producer.
    long long droppedSamples() const noexcept {
        return droppedSamples_.load(std::memory_order_relaxed);
    }
    int       queueHighWater() const noexcept {
        return queueHighWater_.load(std::memory_order_relaxed);
    }
    // Current inbound-queue depth in samples — read by TciServer's
    // CHRONO formula to compute how far behind the target buffer
    // depth we are.  Safe to call from the Qt main thread (same
    // thread that owns the inbound_ deque per the architecture
    // contract above); not safe from any other thread.
    int       currentQueueSize() const noexcept {
        return int(inbound_.size());
    }

    // Fixed 48 kHz — must match the canonical TxDspWorker mic-ring
    // input rate.  TciServer's binary handler resamples if a TCI
    // client streams a non-48k rate.
    static constexpr int sampleRate() { return 48000; }

    // Inbound queue capacity (samples).  Matches the working
    // reference's MAX_TX_AUDIO_QUEUE_COMPLEX_SAMPLES at
    // TCIServer.cs:764 — ≈2 sec at 48 kHz / mono.  Generous,
    // operator-flagged 2026-06-01 as the right ballpark for
    // PureSignal + full-duplex forward-compat.
    static constexpr int kInboundCapacity = 96000;

    // Drain timer cadence (ms) + max samples popped per fire.
    // 10 ms × 48 kHz = 480 samples/fire.  Sized so the final-stage
    // TxRing (8 × kBlockSize = 1024 samples post-§15.29) has plenty
    // of room with the worker draining at the wire's 48 kHz rate.
    static constexpr int kDrainIntervalMs = 10;
    static constexpr int kDrainMaxSamples = 480;

private slots:
    void onDrainTimerFired();

private:
    TxDspWorker            *worker_     = nullptr;
    QTimer                 *drainTimer_ = nullptr;

    // Inbound queue (Qt main thread only — single-producer +
    // single-consumer both on Qt main, no mutex needed).
    std::deque<float>       inbound_;

    std::atomic<long long>  submittedSamples_{0};
    std::atomic<long long>  droppedSamples_{0};
    std::atomic<int>        queueHighWater_{0};
};

} // namespace lyra::dsp
