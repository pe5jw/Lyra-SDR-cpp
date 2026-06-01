// Lyra-cpp — TX-1 component 4c+6: dedicated TX DSP worker thread (impl).
// See tx_dsp_worker.h for the locked operational posture + the
// EP2 hand-off semaphore-pair design.

#include "tx_dsp_worker.h"

#include <QDebug>

#include <algorithm>
#include <chrono>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <avrt.h>
  // Link the AVRT library that provides AvSetMmThreadCharacteristics
  // + AvSetMmThreadPriority + AvRevertMmThreadCharacteristics.
  #pragma comment(lib, "avrt.lib")
#endif

namespace lyra::dsp {

TxDspWorker::TxDspWorker(WdspNative *wdsp, Hl2Ep6MicSource &micSource)
    : tx_(/*channelId=*/1, wdsp)
    // Ring sized for the WORKER'S drain cadence, NOT the producer's
    // burst pattern.  Producer-side burst absorption lives upstream
    // in each MicSource subclass (Hl2Ep6MicSource pushes tiny ~10-
    // sample bursts that fit a small ring trivially; TciMicSource
    // pushes 2400-sample MSHV bursts but holds them in its own
    // inbound queue ≈ 2 sec and drains them in 480-sample chunks
    // every 10 ms — see tci_mic_source.h for the two-stage
    // architecture rationale).
    //
    // 8 × kBlockSize = 512 samples ≈ 10 ms is exactly enough to
    // smooth out wire-side jitter (one drain-timer fire + one
    // worker-pop latency).  Operator-locked 2026-06-01 with the
    // two-stage refactor: "Yo ubeeter make like Thetis because I
    // am pretty sure that this is going to bite are ass when it
    // comes to full duplex and PureSignal!"  Thetis's two-stage
    // TX-audio queue (TCIServer.cs:763-764 inbound queue + small
    // downstream cmaster ring into WDSP TXA) is the pattern Lyra
    // mirrors now — large inbound queue per-source where bursts
    // need absorbing, small final-stage SPSC into the worker.
    //
    // highWater telemetry (TxRing::highWaterSamples) stays — if it
    // ever sustains near 512, that's a real downstream wire-path
    // stall (EP2 timer-paced writer, Qt main thread, etc.), not
    // ring-size mis-tuning.  Producer-burst overruns are now
    // accounted at each MicSource's own dropped-samples counter
    // (TciMicSource::droppedSamples — see tci_mic_source.h
    // diagnostics).
    , ring_(/*capacitySamples=*/8 * kBlockSize, kBlockSize)
    , mic_(micSource)
    , txIqBuf_(static_cast<std::size_t>(kEp2BlockSize),
               std::complex<float>(0.0f, 0.0f))
    , sip1Ring_(kSip1RingSamples,
                std::complex<float>(0.0f, 0.0f))
{
    // Open the WDSP TX channel at the locked design v2 rates:
    // 48 kHz mic in / 96 kHz DSP / 48 kHz I/Q out.  TxChannel::open
    // logs success/failure itself; we additionally bail out of the
    // worker spin-up on a failure so the rest of the app keeps
    // running RX-only.
    if (!tx_.open(48000, 96000, 48000)) {
        qWarning("[tx-worker] TxChannel::open failed; worker NOT spinning up "
                 "(RX path unaffected)");
        return;
    }

    // Pre-size the 64-→-126 accumulator to its worst case
    // (one fresh kBlockSize push + the carryover up to kBlockSize-1
    // = 2*kBlockSize - 1 capacity covers it without realloc on the
    // hot path).
    accum_.reserve(static_cast<std::size_t>(2 * kBlockSize));

    // Hook mic samples into the SPSC ring via the tagged-source
    // dispatch (Task #33).  Capturing `this` is safe — the
    // destructor clears the mic consumer before this object goes
    // away.  The lambda runs SYNCHRONOUSLY on the rx worker thread
    // once per EP6 datagram with ~10 decimated 48k samples;
    // submitMicSamples is lock-free + drops the submission early
    // if the operator has picked a different mic source — preserves
    // the SPSC ring's single-producer contract at the swap.
    mic_.setConsumer([this](const float *samples, int n) {
        submitMicSamples(MicSource::Mic1, samples, n);
    });

    // Spawn the worker thread.  RAII via std::thread; the dtor
    // signals stop + joins.
    worker_ = std::thread(&TxDspWorker::workerLoop, this);

    qInfo("[tx-worker] spawned (TX DSP runs continuously; EP2 hand-off "
          "wire-inert until setInjectTxIq(true) is called by the FSM)");
}

TxDspWorker::~TxDspWorker()
{
    // Order matters — see the header doc block.  This sequence
    // guarantees the worker thread is fully joined BEFORE
    // TxChannel::close runs, so close() cannot race a fexchange0
    // in flight (TxChannel::channelMtx_ would protect against
    // that too, but the explicit sequencing is clearer).
    //
    // Task #40 — TX-triggered zombie shutdown investigation
    // (operator-flagged 2026-05-31, repro: RX-only close clean,
    // TX-touched close leaves zombie).  qWarning at each step so
    // the next bench can show via lyra-log.txt which step (if any)
    // wedged.  Pre-fix qInfo SUMMARY line at the bottom would not
    // print if any step blocks above it — its absence in the log
    // is itself diagnostic data.
    qWarning("[shutdown] ~TxDspWorker ENTRY");
    stopRequested_.store(true, std::memory_order_release);

    // 1) Stop new mic samples landing.  The Hl2Ep6MicSource
    //    destructor will also clear this on its own teardown,
    //    but we do it explicitly here so the order is unambig.
    qWarning("[shutdown] ~TxDspWorker step 1: mic_.setConsumer({}) - start");
    mic_.setConsumer({});
    qWarning("[shutdown] ~TxDspWorker step 1: done");

    // 2) Wake the worker.  shutdown() releases the semaphore
    //    AND flips the ring's shutdown flag, so any in-flight
    //    popBlock returns false on the next iteration.
    qWarning("[shutdown] ~TxDspWorker step 2: ring_.shutdown() - start");
    ring_.shutdown();
    qWarning("[shutdown] ~TxDspWorker step 2: done");

    // 2a) Also un-stick the worker if it's blocked on
    //     txIqConsumed_.acquire() (the EP2-consumed wait).  At
    //     teardown there's no real EP2 consumer left, so we
    //     release the consumed semaphore manually — the worker's
    //     next stopRequested_ check catches it and exits the loop.
    //     Releasing on a wire-inert worker (injectTxIq_ never
    //     became true) is a no-op (nothing's waiting on the
    //     binary semaphore — release just bumps the count to 1,
    //     which is harmless; the dtor frees the object next).
    qWarning("[shutdown] ~TxDspWorker step 2a: txIqConsumed_.release() - start");
    txIqConsumed_.release();
    qWarning("[shutdown] ~TxDspWorker step 2a: done");

    // 3) Join.  Bounded — the worker only ever blocks in
    //    popBlock or txIqConsumed_, both released above.
    qWarning("[shutdown] ~TxDspWorker step 3: worker_.join() - start (joinable=%d)",
             worker_.joinable() ? 1 : 0);
    if (worker_.joinable()) {
        worker_.join();
    }
    qWarning("[shutdown] ~TxDspWorker step 3: done");

    // 4) Now safe to close the TX channel (no more fexchange0
    //    callers).  TxChannel::close holds its own mutex so a
    //    double-close from the destructor chain is idempotent.
    qWarning("[shutdown] ~TxDspWorker step 4: tx_.close() - start");
    tx_.close();
    qWarning("[shutdown] ~TxDspWorker step 4: done");

    qInfo("[tx-worker] stopped (blocks=%lld, errors=%lld, skipped=%lld, "
          "ring-overruns=%lld, ring-high-water=%d/%d, ep2-fills=%lld)",
          blockCount_.load(), errorCount_.load(), skipCount_.load(),
          ring_.overrunCount(),
          ring_.highWaterSamples(), ring_.capacitySamples(),
          sip1FillCount_.load());
    qWarning("[shutdown] ~TxDspWorker EXIT");
}

void TxDspWorker::workerLoop()
{
#ifdef _WIN32
    // MMCSS "Pro Audio" registration — lifts this thread out of
    // the normal scheduler.  Same primitive the working C-source
    // reference's wire-thread pump uses
    // (AvSetMmThreadCharacteristics +
    // AvSetMmThreadPriority(hTask, 2)).  Fallback to
    // THREAD_PRIORITY_HIGHEST if registration fails (rare;
    // happens on some Windows Server SKUs without the
    // multimedia stack).
    DWORD  taskIndex = 0;
    HANDLE hTask     = AvSetMmThreadCharacteristicsW(L"Pro Audio",
                                                     &taskIndex);
    if (hTask != nullptr) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH);
    } else {
        SetThreadPriority(GetCurrentThread(),
                          THREAD_PRIORITY_HIGHEST);
    }
#endif

    // Local scratch — outBlock is heap-allocated once here
    // (NOT on the RT path).  inBlock is on the stack.  Both
    // sized exactly kBlockSize; never resized.
    float                            inBlock[kBlockSize];
    std::vector<std::complex<float>> outBlock(
        static_cast<std::size_t>(kBlockSize),
        std::complex<float>(0.0f, 0.0f));

    while (!stopRequested_.load(std::memory_order_acquire)) {
        // Block until a complete blockSize is ready in the
        // ring, or shutdown returns false.
        if (!ring_.popBlock(inBlock)) {
            break;
        }

        // Task #46 (2026-05-31) — channel-running gate.  When the
        // FSM hasn't keyed yet (RX-only quiescent state), WDSP TXA
        // state is 0 and TxChannel::process() would return -1.  Pre-
        // fix that -1 was counted as an "error" (operator bench:
        // 2097/16182 = 13% spurious "error" rate that was really
        // just popBlocks during RX-only state).  Now we read the
        // atomic isRunning() flag once and skip fexchange0 entirely
        // when not keyed.  Cheap (one atomic acquire-load) and
        // honest in the bench numbers.  Drain the ring (we already
        // popped above — keeps the producer happy) and roll on.
        //
        // The accum drain on the falling-edge of injectTxIq_ below
        // would also fire here, but be explicit: if injectTxIq_ ever
        // got latched true with channel not running (transient
        // start/stop reorder), don't carry stale outputs.
        if (!tx_.isRunning()) {
            skipCount_.fetch_add(1, std::memory_order_relaxed);
            if (!accum_.empty()) {
                accum_.clear();
            }
            continue;
        }

        // Drive the WDSP TXA chain.  TxChannel::process takes
        // its own channelMtx_ internally (lifecycle guard).
        const int err = tx_.process(inBlock, kBlockSize,
                                    outBlock.data());
        blockCount_.fetch_add(1, std::memory_order_relaxed);
        if (err != 0) {
            errorCount_.fetch_add(1, std::memory_order_relaxed);
            continue;   // skip publishing on a non-zero status
        }

        // EP2 hand-off gate.  injectTxIq_ defaults FALSE and stays
        // false until the FSM keydown wires it true (a follow-up
        // commit).  While false: discard the WDSP output here —
        // do NOT accumulate, do NOT signal dataReady, no SSB I/Q
        // can land on the wire by construction.  Wire-inert.
        if (!injectTxIq_.load(std::memory_order_acquire)) {
            // Keep the accumulator drained while inert so when
            // injectTxIq_ eventually flips true we don't dump
            // stale samples from before the keydown.
            if (!accum_.empty()) {
                accum_.clear();
            }
            continue;
        }

        // Accumulate this 64-sample fexchange0 output into the
        // EP2-sized buffer.  Worst case: previous round-trip
        // left up to kBlockSize-1 carryover; we add kBlockSize
        // here; total ≤ 2*kBlockSize.
        accum_.insert(accum_.end(), outBlock.begin(), outBlock.end());

        // Hand off every full kEp2BlockSize-sized chunk to the
        // EP2 consumer.  The loop covers the (rare) case where
        // a burst of mic samples produced two fexchange0 outputs
        // back-to-back and the accumulator now has ≥ 2*kEp2BlockSize.
        //
        // Task #40 — TX-triggered zombie shutdown ROOT-CAUSE FIX
        // (operator bench 2026-05-31): without the stopRequested_
        // check inside this inner loop, teardown deadlocked here.
        // Scenario:
        //   1. Worker keyed (accum_.size() >= kEp2BlockSize).
        //   2. signalAndWait blocked on txIqConsumed_.acquire().
        //   3. Dtor sets stopRequested_=true + releases the
        //      semaphore ONCE to unstick the worker.
        //   4. signalAndWait's acquire succeeds, sees stopRequested_,
        //      EARLY-RETURNS without erasing from accum_.
        //   5. Outer while sees accum_.size() unchanged + re-enters
        //      signalAndWait → acquire blocks forever (no more
        //      releases coming from the now-gone dtor).
        // Checking stopRequested_ BEFORE re-entering signalAndWait
        // breaks the loop on the same iteration that the early-
        // return happened.  Bench-confirmed wedge point via watchdog
        // diagnostic trace (commit 65c965d).
        while (static_cast<int>(accum_.size()) >= kEp2BlockSize) {
            if (stopRequested_.load(std::memory_order_acquire)) break;
            signalAndWaitForEp2Consumer();
        }
    }

#ifdef _WIN32
    if (hTask != nullptr) {
        AvRevertMmThreadCharacteristics(hTask);
    }
#endif
}

void TxDspWorker::signalAndWaitForEp2Consumer() noexcept
{
    // Wait for the previous in-flight buffer to be CONSUMED before
    // we overwrite txIqBuf_.  Reference-faithful match to the
    // verified reference's
    //   WaitForSingleObject(hobbuffsRun[0], INFINITE)
    // — INFINITE wait, matching the reference's strict producer-
    // consumer lockstep (see docs/architecture/tx1_ssb_design.md
    // §5.7 for the cite).  Bounded in practice by the EP2 wire
    // cadence (~2.6 ms per datagram).  On teardown the dtor
    // releases txIqConsumed_ to unstick us; the stopRequested_
    // check immediately below catches it and returns without
    // touching the shared buffer or signalling dataReady.
    //
    // Underrun accounting lives on the CONSUMER side (HL2Stream
    // EP2 packer increments its own counter when SSB was expected
    // but tryConsumeTxIq returned false).  This is the natural
    // ownership: underrun is the wire's perspective on missing
    // samples, not the producer's.
    txIqConsumed_.acquire();

    if (stopRequested_.load(std::memory_order_acquire)) {
        // Stop requested while we were waiting.  Don't touch the
        // shared buffer, don't signal dataReady.
        //
        // Task #40 — TX-triggered zombie shutdown ROOT-CAUSE FIX
        // (operator bench 2026-05-31): clear accum_ here as
        // defense-in-depth.  The outer workerLoop while-loop now
        // ALSO checks stopRequested_ before re-entering this
        // function (see workerLoop), so the loop terminates
        // regardless.  This clear is a second safety net: any
        // future caller that doesn't add the outer-loop check
        // (refactor, new entry point, etc.) still terminates
        // because the accumulator drains, the
        // `accum.size() >= kEp2BlockSize` condition fails, and the
        // outer while exits naturally.  Cost: drops up to
        // kEp2BlockSize-1 samples of TX audio that would have
        // gone on the wire — irrelevant at teardown, the operator
        // already keyed up.
        accum_.clear();
        return;
    }

    // Fill the shared buffer from the front of the accumulator.
    // Matches the verified reference's
    //   memcpy(outIQbufp, out, sizeof(complex) * 126)
    // (see docs/architecture/tx1_ssb_design.md §5.7 for the cite).
    std::copy(accum_.begin(),
              accum_.begin() + kEp2BlockSize,
              txIqBuf_.begin());
    accum_.erase(accum_.begin(),
                 accum_.begin() + kEp2BlockSize);

    // Tee the same 126-sample chunk into the sip1 ring for v0.3
    // PureSignal forward-compat.  No consumer in v0.2; calcc reads
    // this at predistortion-calibration time when v0.3 lands.
    // Wraparound is by simple modulo on the write index — the
    // ring has no read index in v0.2 (oldest data drops by
    // overwrite, which is exactly what PS calcc wants).
    const std::size_t base = sip1WriteIdx_.load(std::memory_order_relaxed);
    for (int i = 0; i < kEp2BlockSize; ++i) {
        sip1Ring_[(base + static_cast<std::size_t>(i)) % kSip1RingSamples]
            = txIqBuf_[i];
    }
    sip1WriteIdx_.store((base + static_cast<std::size_t>(kEp2BlockSize))
                            % kSip1RingSamples,
                        std::memory_order_relaxed);
    sip1FillCount_.fetch_add(1, std::memory_order_relaxed);

    // Signal "data ready".  Matches the verified reference's
    //   ReleaseSemaphore(hsendIQSem, 1, 0)
    // (see docs/architecture/tx1_ssb_design.md §5.7 for the cite).
    txIqDataReady_.release();
}

bool TxDspWorker::tryConsumeTxIq(std::complex<float> *out) noexcept
{
    if (!out) return false;
    // Non-blocking try-acquire.  If injectTxIq_ is false the
    // producer never released dataReady → this always returns
    // false → consumer zero-fills (mandatory zero-on-no-MOX per
    // §5.7 + the verified reference's universal `!XmitBit ⇒ zero`
    // posture; see docs/architecture/tx1_ssb_design.md §5.7 for
    // the cite).  Wire-inert by construction until the FSM
    // wires injectTxIq_.
    if (!txIqDataReady_.try_acquire()) {
        return false;
    }
    // We hold the data-ready guarantee.  Copy the 126 samples out
    // of the shared buffer and release consumed so the producer
    // can refill next round.  Matches the verified reference's
    //   pack to wire bytes ; ReleaseSemaphore(hobbuffsRun[0], 1, 0)
    // (see docs/architecture/tx1_ssb_design.md §5.7 for the cite).
    std::copy(txIqBuf_.begin(),
              txIqBuf_.begin() + kEp2BlockSize,
              out);
    txIqConsumed_.release();
    return true;
}

} // namespace lyra::dsp
