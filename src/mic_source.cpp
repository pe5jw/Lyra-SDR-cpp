// Lyra-cpp — TX-1 component 3: Hl2Ep6MicSource (impl).

#include "mic_source.h"

namespace lyra::dsp {

Hl2Ep6MicSource::Hl2Ep6MicSource(lyra::ipc::HL2Stream &stream)
    : stream_(stream)
{
    // Install ourselves unconditionally at construction.  The
    // forwarder forwards to the operator's downstream `consumer_`
    // if one is registered; drops samples on the floor otherwise.
    // Capturing `this` is safe — the destructor clears the
    // HL2Stream-side callback before this goes away.
    stream_.setMicConsumer(
        [this](const float *samples, int n) {
            if (consumer_) {
                consumer_(samples, n);
            }
        });
}

Hl2Ep6MicSource::~Hl2Ep6MicSource()
{
    // Clear the HL2Stream-side callback FIRST so the RX worker
    // thread can't be inside our lambda when this object is
    // destroyed.  After this returns, HL2Stream stores a default-
    // constructed (empty) std::function — the `if (micConsumer_)`
    // guard in the RX loop becomes false and forwarding stops.
    // NOTE: this is only safe if HL2Stream::setMicConsumer is
    // called from the same thread that calls our destructor AND
    // the RX worker either is not running or guarantees not to
    // re-enter the callback after the swap.  Since we follow the
    // setIqSink contract — set ONCE before open() spawns the
    // worker — operators tear us down before HL2Stream::stop()
    // joins the worker.  See header threading note.
    //
    // Task #40 — TX-triggered zombie shutdown investigation.
    // setMicConsumer({}) takes the consumer mutex on HL2Stream,
    // which BLOCKS until any in-flight RX-loop micConsumer_ call
    // finishes — candidate wedge point if the RX worker thread is
    // stuck in the middle of forwarding mic samples.  qWarning
    // brackets so lyra-log.txt shows entry/exit timing.
    qWarning("[shutdown] ~Hl2Ep6MicSource ENTRY");
    stream_.setMicConsumer({});
    qWarning("[shutdown] ~Hl2Ep6MicSource EXIT");
}

void Hl2Ep6MicSource::setConsumer(Consumer c)
{
    // Same single-threaded set-once-before-start contract as
    // HL2Stream::setIqSink — no lock, no swap dance.  Callers
    // honour the contract; if a future use case needs live
    // re-registration mid-run, that lands with a
    // std::atomic<std::shared_ptr<Consumer>> swap, not here.
    consumer_ = std::move(c);
}

} // namespace lyra::dsp
