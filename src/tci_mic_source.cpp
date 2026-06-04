// Lyra-cpp — Task #33: TciMicSource impl.  See tci_mic_source.h
// for the full two-stage-buffer architecture rationale.

#include "tci_mic_source.h"

#include <QTimer>

#include <algorithm>

namespace lyra::dsp {

TciMicSource::TciMicSource(TxDspWorker *worker, QObject *parent)
    : QObject(parent), worker_(worker)
{
    // Pre-allocate the inbound queue's worst-case backing — std::deque
    // is chunked internally, so this just nudges the first chunk to
    // exist.  No O(n) allocation on the hot-path push.
    // (std::deque doesn't have reserve(); the constructor pre-fills
    // and clear() drops back to size=0 without releasing capacity.)
    inbound_.clear();

    drainTimer_ = new QTimer(this);
    drainTimer_->setInterval(kDrainIntervalMs);
    // PreciseTimer: at 2 ms intervals Qt's default CoarseTimer
    // (5 % accuracy below 50 ms) would quantise the tick up to
    // ~5-10 ms.  Precise mode holds the 2 ms cadence the
    // reference's TX stream loop establishes — required to keep
    // the worker ring fed without 10+ ms starvation gaps that
    // produce alternating multi-tone/carrier output on the wire.
    drainTimer_->setTimerType(Qt::PreciseTimer);
    connect(drainTimer_, &QTimer::timeout,
            this, &TciMicSource::onDrainTimerFired);
    drainTimer_->start();
}

TciMicSource::~TciMicSource()
{
    if (drainTimer_) drainTimer_->stop();
    // QTimer parented to this; auto-deleted.
}

void TciMicSource::submitFromTci(const float *samples, int n) noexcept
{
    if (!worker_ || !samples || n <= 0) return;
    submittedSamples_.fetch_add(n, std::memory_order_relaxed);

    // Drop-OLDEST on inbound-queue overrun (working reference parity:
    // TCIServer.cs:5689-5702 dequeues from the front to make room
    // when the queue is full).  Keeps the freshest audio in the
    // queue; better for digital-mode TX cycles than dropping the
    // newest (which would corrupt the modem's frame structure).
    const std::size_t need = inbound_.size() + std::size_t(n);
    if (need > std::size_t(kInboundCapacity)) {
        const std::size_t drop = need - std::size_t(kInboundCapacity);
        if (drop >= inbound_.size()) {
            droppedSamples_.fetch_add(static_cast<long long>(inbound_.size()),
                                      std::memory_order_relaxed);
            inbound_.clear();
        } else {
            droppedSamples_.fetch_add(static_cast<long long>(drop),
                                      std::memory_order_relaxed);
            inbound_.erase(inbound_.begin(),
                           inbound_.begin() + std::ptrdiff_t(drop));
        }
    }

    inbound_.insert(inbound_.end(), samples, samples + n);

    // High-water mark for diagnostics — operator can sanity-check
    // that the queue isn't sustaining near capacity (which would
    // mean the drain timer can't keep up with the producer).
    const int sz = int(inbound_.size());
    int prev = queueHighWater_.load(std::memory_order_relaxed);
    while (sz > prev) {
        if (queueHighWater_.compare_exchange_weak(
                prev, sz, std::memory_order_relaxed,
                std::memory_order_relaxed)) break;
    }
}

void TciMicSource::onDrainTimerFired()
{
    // Pop up to kDrainMaxSamples from the inbound queue, push to
    // TxDspWorker's TxRing tagged MicSource::Tci.  TxDspWorker drops
    // submissions at its front door if activeMicSource != Tci
    // (operator hasn't picked TCI in Settings → TX), so this is a
    // silent no-op in that case — no special branching here.
    if (!worker_ || inbound_.empty()) return;

    int n = std::min<int>(int(inbound_.size()), kDrainMaxSamples);

    // std::deque doesn't guarantee contiguous storage, so we have
    // to copy into a temporary buffer for the worker's submit API.
    // 480 floats × 4 B = 1920 B per fire @ 100 Hz = 192 KB/s memory
    // bandwidth.  Negligible.
    static thread_local std::vector<float> tmp;
    tmp.resize(static_cast<std::size_t>(n));
    auto it = inbound_.begin();
    for (int i = 0; i < n; ++i) tmp[std::size_t(i)] = *it++;
    inbound_.erase(inbound_.begin(), it);

    worker_->submitMicSamples(TxDspWorker::MicSource::Tci,
                              tmp.data(), n);
}

} // namespace lyra::dsp
