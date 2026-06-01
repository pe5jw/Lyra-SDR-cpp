// Lyra-cpp — Task #33: TciMicSource.
//
// Sibling of Hl2Ep6MicSource.  Where Hl2Ep6MicSource decimates the
// HL2 EP6 codec mic bytes and forwards them to TxDspWorker, this
// class forwards inbound TCI v2 TX_AUDIO_STREAM samples (decoded by
// TciServer's binary-frame handler) into the SAME TxDspWorker mic
// ring, tagged with MicSource::Tci.
//
// Operator workflow: choose "TCI" in Settings → TX → Mic Source (or
// let MSHV / any TCI client send `trx:0,true,tci`, which auto-flips
// the source).  TxDspWorker drops any inactive-source submissions at
// its front door, so submissions from this class are silent no-ops
// unless `activeMicSource() == MicSource::Tci`.
//
// First-light client: MSHV (FT8 / FT4 / MSK144 / Q65 / ...).  Per
// docs/refs/mshv_tci/README.md, MSHV streams FLOAT32 mono @ 48 kHz
// in 2048-sample blocks with a 50 ms TX-buffering hint.
//
// Threading: TciServer runs on the Qt main thread (QWebSocketServer
// is signal/slot driven), so submitFromTci() is called from the
// main thread.  TxDspWorker's submitMicSamples is lock-free under
// the strict-SPSC contract — preserved here because only ONE source
// is active at a time (mic1 producer = HL2 rx-worker thread; tci
// producer = Qt main thread; they're NEVER both active).

#pragma once

#include "tx_dsp_worker.h"

#include <QObject>

namespace lyra::dsp {

class TciMicSource : public QObject {
    Q_OBJECT
public:
    // Construct with a back-pointer to the TxDspWorker we feed.
    // TxDspWorker must outlive this object (caller arranges
    // teardown order: TciMicSource destroyed BEFORE TxDspWorker,
    // mirroring the existing Hl2Ep6MicSource ordering in main.cpp).
    explicit TciMicSource(TxDspWorker *worker, QObject *parent = nullptr);
    ~TciMicSource() override = default;

    TciMicSource(const TciMicSource &)            = delete;
    TciMicSource &operator=(const TciMicSource &) = delete;

    // Push N float32 mono samples @ 48 kHz into the TxDspWorker
    // mic ring, tagged MicSource::Tci.  Caller (TciServer's binary
    // frame handler) is responsible for any sample-rate / format /
    // channel-count conversion BEFORE calling here — this API takes
    // the canonical TX-ring format (FLOAT32 mono 48 kHz).  Returns
    // immediately if the worker isn't connected.  No-op if the
    // operator-selected mic source isn't TCI (drop happens inside
    // TxDspWorker::submitMicSamples — single source-of-truth).
    void submitFromTci(const float *samples, int n) noexcept;

    // Diagnostics — total samples submitted (regardless of whether
    // they were actually admitted to the ring).  Operator can
    // compare against TxDspWorker::overrunCount to see whether TCI
    // is dropping at the front door (source-mismatch) vs the ring
    // back door (overrun).
    long long submittedSamples() const noexcept {
        return submittedSamples_.load(std::memory_order_relaxed);
    }

    // Fixed 48 kHz — must match the canonical TxDspWorker mic-ring
    // input rate.  TciServer's binary handler resamples if a TCI
    // client streams a non-48k rate (Task #33 commit 2).
    static constexpr int sampleRate() { return 48000; }

private:
    TxDspWorker            *worker_ = nullptr;
    std::atomic<long long>  submittedSamples_{0};
};

} // namespace lyra::dsp
