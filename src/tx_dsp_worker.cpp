// Lyra-cpp — TX-1 component 4c: dedicated TX DSP worker thread (impl).
// See tx_dsp_worker.h for the locked operational posture.

#include "tx_dsp_worker.h"

#include <QDebug>

#include <algorithm>

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
    , ring_(/*capacitySamples=*/8 * kBlockSize, kBlockSize)
    , mic_(micSource)
    , latestBlock_(static_cast<size_t>(kBlockSize),
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

    // Hook mic samples into the SPSC ring.  Capturing `this` is
    // safe — the destructor clears the mic consumer before this
    // object goes away.  The lambda runs SYNCHRONOUSLY on the rx
    // worker thread once per EP6 datagram with ~10 decimated 48k
    // samples; ring_.push is lock-free under the strict-SPSC
    // contract.
    mic_.setConsumer([this](const float *samples, int n) {
        ring_.push(samples, n);
    });

    // Spawn the worker thread.  RAII via std::thread; the dtor
    // signals stop + joins.
    worker_ = std::thread(&TxDspWorker::workerLoop, this);

    qInfo("[tx-worker] spawned (TX DSP runs continuously; output gated only "
          "at the wire pack when component 6 lands)");
}

TxDspWorker::~TxDspWorker()
{
    // Order matters — see the header doc block.  This sequence
    // guarantees the worker thread is fully joined BEFORE
    // TxChannel::close runs, so close() cannot race a fexchange0
    // in flight (TxChannel::channelMtx_ would protect against
    // that too, but the explicit sequencing is clearer).
    stopRequested_.store(true, std::memory_order_release);

    // 1) Stop new mic samples landing.  The Hl2Ep6MicSource
    //    destructor will also clear this on its own teardown,
    //    but we do it explicitly here so the order is unambig.
    mic_.setConsumer({});

    // 2) Wake the worker.  shutdown() releases the semaphore
    //    AND flips the ring's shutdown flag, so any in-flight
    //    popBlock returns false on the next iteration.
    ring_.shutdown();

    // 3) Join.  Bounded — the worker only ever blocks in
    //    popBlock, which now returns false.
    if (worker_.joinable()) {
        worker_.join();
    }

    // 4) Now safe to close the TX channel (no more fexchange0
    //    callers).  TxChannel::close holds its own mutex so a
    //    double-close from the destructor chain is idempotent.
    tx_.close();

    qInfo("[tx-worker] stopped (blocks=%lld, errors=%lld, "
          "ring-overruns=%lld)",
          blockCount_.load(), errorCount_.load(),
          ring_.overrunCount());
}

void TxDspWorker::workerLoop()
{
#ifdef _WIN32
    // MMCSS "Pro Audio" registration — lifts this thread out of
    // the normal scheduler.  Same primitive the C reference's
    // cm_main pump uses (AvSetMmThreadCharacteristics +
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
        static_cast<size_t>(kBlockSize),
        std::complex<float>(0.0f, 0.0f));

    while (!stopRequested_.load(std::memory_order_acquire)) {
        // Block until a complete blockSize is ready in the
        // ring, or shutdown returns false.
        if (!ring_.popBlock(inBlock)) {
            break;
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
        // Publish the latest TX I/Q block for component 6.
        // Brief lock; the copy is small (126 complex<float> =
        // 1 KiB).
        {
            std::lock_guard<std::mutex> lk(latestMtx_);
            std::copy(outBlock.begin(), outBlock.end(),
                      latestBlock_.begin());
            latestBlockSize_ = kBlockSize;
        }
    }

#ifdef _WIN32
    if (hTask != nullptr) {
        AvRevertMmThreadCharacteristics(hTask);
    }
#endif
}

int TxDspWorker::latestTxBlock(std::complex<float> *out,
                               int maxFrames) const
{
    if (!out || maxFrames <= 0) return 0;
    std::lock_guard<std::mutex> lk(latestMtx_);
    // Ternary instead of std::min — matches the C reference's
    // Windows-min-macro / inline-ternary idiom (no NOMINMAX dance,
    // no std::min vs Windows macro conflict).
    const int n = (maxFrames < latestBlockSize_) ? maxFrames
                                                 : latestBlockSize_;
    if (n <= 0) return 0;
    std::copy(latestBlock_.begin(),
              latestBlock_.begin() + n,
              out);
    return n;
}

} // namespace lyra::dsp
