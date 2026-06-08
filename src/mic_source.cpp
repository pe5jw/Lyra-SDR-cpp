// Lyra-cpp — TX-1 component 3: Hl2Ep6MicSource (impl).
//
// Stage 2b2c: migrated from the retired `HL2Stream::setMicConsumer`
// shim to `Ep6RecvThread::set_mic_sink(...)` direct.  Reference-
// faithful end-to-end: the wire-side sink callback signature
// `(int n_samples, const double* iq_pairs)` matches the reference's
// `Inbound(int id, int nsamples, double* in)` contract, and the
// `iq_pairs` buffer is interleaved {I = mic_double, Q = 0.0} pairs
// in [-1, +1) byte-shape-identical to `prn->TxReadBufp` — passed
// through to the operator-facing Consumer unchanged.  No float
// bridge, no Q6.5 RMS bench instrument, no shutdown-mutex
// safetyLog — none of those exist in the reference path.

#include "mic_source.h"

namespace lyra::dsp {

Hl2Ep6MicSource::Hl2Ep6MicSource(lyra::ipc::HL2Stream &stream)
    : stream_(stream)
{
    // Install ourselves on the Ep6 reader thread's mic sink.  This
    // runs at app startup, BEFORE HL2Stream::open() starts
    // ep6Thread_ — `Ep6RecvThread::set_mic_sink` enforces the
    // set-once-before-start invariant (F1 `assert_not_running`).
    //
    // Capturing `this` is safe — the dtor clears the sink under
    // the same not-running invariant (after HL2Stream::close()
    // has joined the Ep6 reader), so the reader thread cannot be
    // inside our lambda when this object is destroyed.
    stream_.ep6Thread().set_mic_sink(
        [this](int n_samples, const double *iq_pairs) {
            if (consumer_) {
                consumer_(n_samples, iq_pairs);
            }
        });
}

Hl2Ep6MicSource::~Hl2Ep6MicSource()
{
    // Symmetric to the ctor: clear the wire-side sink under the
    // set-once-before-start invariant.  At process shutdown this
    // dtor runs from main.cpp's aboutToQuit handler-3, which is
    // registered AFTER handler-2 (`stream->close()` — joins
    // ep6Thread_).  Qt fires aboutToQuit handlers in registration
    // order, so ep6Thread_ is provably joined here and the reader
    // cannot be inside the lambda.  This is the exact discipline
    // documented at Ep6RecvThread.cpp:305-311.
    stream_.ep6Thread().set_mic_sink({});
}

void Hl2Ep6MicSource::setConsumer(Consumer c)
{
    // Same single-threaded set-once-before-start contract — no
    // lock, no swap dance.  Callers honour the contract; if a
    // future use case needs live re-registration mid-run, that
    // lands with the wire-side sink's contract change, not here.
    consumer_ = std::move(c);
}

} // namespace lyra::dsp
