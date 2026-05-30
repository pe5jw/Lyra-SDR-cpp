// Lyra-cpp — TX-1 component 3: Hl2Ep6MicSource.
//
// Thin Lyra-native shim wrapping lyra::ipc::HL2Stream's mic-byte forwarder.
// The producer (lyra::ipc::HL2Stream's RX worker) has already done all the
// hard work: extracted the 16-bit BE mic sample from bytes [24..25]
// of each 26-byte EP6 slot, normalised to [-1, +1), and decimated
// to exactly 48 kHz per the current wire rate (the C reference's
// `mic_decimation_factor` table — see lyra::ipc::HL2Stream::setSampleRate +
// the rx loop).  This class just provides an operator-facing
// `Consumer` callback API so the eventual TX DSP worker
// (component 4) can register a single SPSC-ring push as the
// destination — no other indirection added.
//
// Construct AT STARTUP, unconditionally (same posture as the
// reference's create_xmtr): lyra::ipc::HL2Stream then forwards mic blocks
// every datagram regardless of MOX state.  When the TX DSP worker
// later sets its Consumer, samples start flowing into the ring;
// when no Consumer is set, the data is silently dropped (the
// lyra::ipc::HL2Stream-side callback is a no-op).
//
// Threading: setConsumer() must be called before lyra::ipc::HL2Stream.open()
// spawns the worker, mirroring lyra::ipc::HL2Stream::setIqSink()'s contract.
// Once running, the consumer runs SYNCHRONOUSLY on the RX worker
// thread once per EP6 datagram — must be lock-free / sub-µs work.

#pragma once

#include "hl2_stream.h"

#include <functional>

namespace lyra::dsp {

class Hl2Ep6MicSource {
public:
    // Mic samples arrive in mono float32 blocks at 48 kHz.  `n`
    // varies (≈ wire_rate / 48000 samples-per-datagram per the
    // wire rate the radio is configured at — ~10 at 192 kHz nddc=4
    // since each datagram carries 38 mic-bearing slots / factor 4).
    using Consumer = std::function<void(const float *samples, int n)>;

    // Construct + install ourselves as the lyra::ipc::HL2Stream's single mic
    // consumer.  lyra::ipc::HL2Stream::setMicConsumer must not be touched by
    // anyone else for the lifetime of this object.
    explicit Hl2Ep6MicSource(lyra::ipc::HL2Stream &stream);
    ~Hl2Ep6MicSource();

    Hl2Ep6MicSource(const Hl2Ep6MicSource &)            = delete;
    Hl2Ep6MicSource &operator=(const Hl2Ep6MicSource &) = delete;

    // Register the operator's downstream consumer.  Call once,
    // before lyra::ipc::HL2Stream::open() spawns its worker (the
    // setMicConsumer contract).  Pass {} to clear.
    void setConsumer(Consumer c);

    // Fixed 48 kHz — the AK4951 codec rate the gateware locks to
    // and the rate lyra::ipc::HL2Stream's decimator targets for any wire rate.
    static constexpr int sampleRate() { return 48000; }

private:
    lyra::ipc::HL2Stream &stream_;
    Consumer   consumer_;
};

} // namespace lyra::dsp
