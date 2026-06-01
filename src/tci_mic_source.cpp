// Lyra-cpp — Task #33: TciMicSource impl.  See tci_mic_source.h.

#include "tci_mic_source.h"

namespace lyra::dsp {

TciMicSource::TciMicSource(TxDspWorker *worker, QObject *parent)
    : QObject(parent), worker_(worker)
{
}

void TciMicSource::submitFromTci(const float *samples, int n) noexcept
{
    if (!worker_ || !samples || n <= 0) return;
    submittedSamples_.fetch_add(n, std::memory_order_relaxed);
    // The tag-and-drop dispatch lives inside TxDspWorker — single
    // source of truth.  If the operator hasn't selected TCI as the
    // mic source, this submission is dropped at the front door
    // (no ring push, no SPSC contract violation).
    worker_->submitMicSamples(TxDspWorker::MicSource::Tci, samples, n);
}

} // namespace lyra::dsp
