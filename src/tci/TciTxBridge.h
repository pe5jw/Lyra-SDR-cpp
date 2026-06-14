// Lyra — TCI TX audio bridge (§7 / §10.2 tci layer; §4.6 data model).
//
// Host-side sink for inbound TCI transmit audio → the verbatim
// ChannelMaster TX pump.  Re-home of the TX-rip-removed TciMicSource
// (TX_ARCHITECTURAL_MAPPING.md §4.6 `m_tciTxSampleQueue`).  A TCI client
// (MSHV / JTDX / WSJT-X, etc.) streams TX_AUDIO_STREAM frames; TciServer
// decodes + applies the operator gain (#108) + resamples to the TXA input
// rate (#68) and pushes the resulting mono floats here (pushMono).  The
// WDSP `cm_main` TX pump drains them through the reference
// `InboundTCITxAudio` seam (CMaster.cpp xcmaster case 1, gated on
// `use_tci_audio`): `inboundCb` fills `pcm->in[stream]` as I=Q=mono
// complex (Task #67), zero-fill on underrun (reference cmaster.cs:
// 1806-1810).  CHRONO (#64) reads queuedSamples() for its dynamic-pull
// backpressure.

#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace lyra::tci {

class TciTxBridge {
public:
    static TciTxBridge &instance();

    // Producer side (TciServer thread): append resampled mono samples at
    // the TXA input rate.
    void pushMono(const std::vector<float> &mono);

    // Current queued mono-sample depth — consumed by the CHRONO pull
    // formula (TciServer::onChronoTick) so it backpressures the client.
    int queuedSamples() const;

    // Drop all queued audio (TX-source change / defensive).
    void clear();

    // C-shaped callback registered with the WDSP seam via
    // `SendpInboundTCITxAudio`.  Drains `nsamples` complex frames into the
    // interleaved-I/Q `buff` (I=Q=mono), zero-fill on underrun.  Runs on
    // the cm_main TX pump thread.
    static void inboundCb(int nsamples, double *buff);

private:
    TciTxBridge() = default;
    void drainInto(int nsamples, double *buff);

    mutable std::mutex mtx_;
    std::deque<float>  q_;
    // Belt-and-braces cap: the reference queue is CHRONO-backpressured and
    // nominally unbounded; this only bounds a misbehaving / disconnecting
    // client (drop-oldest).  ~4 s at 48 kHz.
    static constexpr std::size_t kCapSamples = 48000 * 4;
};

}  // namespace lyra::tci
