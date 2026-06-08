// Lyra-cpp — TX-1 component 3: Hl2Ep6MicSource.
//
// Thin Lyra-native shim wrapping `Ep6RecvThread::set_mic_sink(...)`.
// The producer (Ep6RecvThread) has already done all the hard work in
// reference-faithful form: per-slot decimation against
// `mic_decimation_factor`, BE 16-bit sign-extension to 32-bit, scale
// by `1.0/2^31` → double in [-1.0, +1.0), and write into the
// `prn->TxReadBufp` scratch as interleaved {I = mic_double, Q = 0.0}
// pairs, then deliver once per EP6 datagram via the sink callback —
// byte-shape-identical to the reference's
// `Inbound(inid(1, 0), mic_sample_count, prn->TxReadBufp)` call.
//
// This class provides the operator-facing `Consumer` callback so a
// downstream TX DSP worker can plug in.  The Consumer signature
// matches the wire-side sink contract (and the reference's Inbound
// signature) — doubles end-to-end, no float bridge: WDSP's
// `fexchange0` takes doubles on the TX path, and the reference flows
// the mic samples as doubles from `Inbound` through the CMB ring +
// cmaster pump straight into `fexchange0(chid(stream,0),
// pcm->in[stream] /*double**/, …)`.
//
// Construct at startup: the Consumer wiring lands on `Ep6RecvThread`
// before `HL2Stream::open()` starts that thread, satisfying the
// `Ep6RecvThread::set_mic_sink` set-once-before-start contract
// (F1: `assert_not_running`).  Operator-facing
// `setConsumer({...})` may be called any time before the same
// open() boundary.  After teardown — i.e. after `HL2Stream::close()`
// has joined `Ep6RecvThread` — the dtor clears the wire-side sink
// for symmetry, also under the not-running invariant.
//
// Threading: the registered Consumer is invoked SYNCHRONOUSLY on
// the Ep6 reader thread once per EP6 datagram with the
// `prn->TxReadBufp` pointer.  Must be lock-free / sub-µs work, and
// the data is valid only for the duration of the call (the next
// datagram overwrites the buffer).

#pragma once

#include "hl2_stream.h"

#include <functional>

namespace lyra::dsp {

class Hl2Ep6MicSource {
public:
    // Mic samples arrive as interleaved {I = mic_double, Q = 0.0}
    // pairs at 48 kHz (already decimated from the wire mic rate by
    // `mic_decimation_factor`).  `n_samples` = number of (I,Q) pairs;
    // the buffer length is `2 * n_samples` doubles.  Range [-1, +1).
    // Shape matches the reference's
    // `Inbound(int id, int nsamples, double* in)` contract (cmbuffs.c).
    using Consumer = std::function<void(int n_samples, const double *iq_pairs)>;

    // Construct + install ourselves as Ep6RecvThread's single mic
    // sink.  `Ep6RecvThread::set_mic_sink` must not be touched by
    // anyone else for the lifetime of this object.  Both register
    // and unregister happen with the EP6 reader thread NOT running
    // (set-once-before-start contract).
    explicit Hl2Ep6MicSource(lyra::ipc::HL2Stream &stream);
    ~Hl2Ep6MicSource();

    Hl2Ep6MicSource(const Hl2Ep6MicSource &)            = delete;
    Hl2Ep6MicSource &operator=(const Hl2Ep6MicSource &) = delete;

    // Register the operator's downstream consumer.  Call once,
    // before `HL2Stream::open()` starts the Ep6 reader thread.
    // Pass {} to clear.
    void setConsumer(Consumer c);

    // Fixed 48 kHz — the AK4951 codec rate the gateware locks to
    // and the rate Ep6RecvThread's decimator targets for any wire
    // rate.
    static constexpr int sampleRate() { return 48000; }

private:
    lyra::ipc::HL2Stream &stream_;
    Consumer              consumer_;
};

} // namespace lyra::dsp
