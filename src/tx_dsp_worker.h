// Lyra-cpp — TX-1 component 4c: dedicated TX DSP worker thread.
//
// Owns one TxChannel + one TxRing + one std::thread.  Wired at
// construction:
//
//   mic samples (rx loop, 48 kHz via Hl2Ep6MicSource)
//        │ producer push
//        ▼
//   TxRing (SPSC, 8x blockSize, semaphore-signalled)
//        │ consumer popBlock
//        ▼
//   TX worker thread (MMCSS "Pro Audio" prio 2)
//        │ calls TxChannel::process()
//        ▼
//   fexchange0 → WDSP TXA chain → TX I/Q output
//        │ publish latest block
//        ▼
//   latestBlock_ (read by Component 6 — EP2 packer — when it lands)
//
// THE LOCKED OPERATIONAL POSTURE (design v2 §5.5, source-verified):
//
//   * Constructed AT STARTUP UNCONDITIONALLY.  Matches the
//     working C-source reference's TX-create + wire-thread pump
//     pattern: TX DSP machinery is created at boot and runs
//     CONTINUOUSLY.  Output is gated only at the wire-pack stage
//     (Component 6's `if (!mox) memset(outIQbufp, 0, ...)` at the
//     EP2 packer — the §15.25-locked behaviour; see
//     docs/architecture/tx1_ssb_design.md for the wire-protocol
//     C-reference cite).  This trades ~1-3% sustained CPU for
//     zero latency on first key-down — the operator-locked design
//     call.
//
//   * Worker thread runs at MMCSS "Pro Audio" priority 2 — lifts
//     it out of the normal scheduler so a Qt main-thread paint
//     storm or a GC pause cannot starve the fexchange0 cadence.
//     Same primitive the working C-source reference's wire-thread
//     pump uses.
//
//   * TxChannel's own channelMtx_ is the lifecycle guard
//     (analogue of the C-reference per-stream update lock).
//     WDSP's INTERNAL csDSP/csEXCH separately serialises
//     fexchange0 against operator setters — we don't replicate
//     that lock here.
//
// HISTORICAL CONTEXT — earlier in-session attempts at the worker
// thread crashed twice with SPSC violations + drop-oldest races.
// THOSE PATTERNS ARE STRUCTURALLY IMPOSSIBLE IN THIS DESIGN:
//   * TxRing has NO drop-oldest (overrun = drop new + count); the
//     producer NEVER mutates outIdx_.
//   * Ring is sized 8x blockSize so steady-state overrun is
//     structurally unreachable.
//   * Worker drains in fixed blockSize chunks; producer pushes
//     decimated 48 kHz mic samples per datagram.
//   * The mutex on TxChannel covers the lifecycle race that bit
//     the v2 attempt (process() running into a half-torn-down
//     channel during shutdown).

#pragma once

#include "mic_source.h"
#include "tx_channel.h"
#include "tx_ring.h"
#include "wdsp_native.h"

#include <atomic>
#include <complex>
#include <mutex>
#include <thread>
#include <vector>

namespace lyra::dsp {

class TxDspWorker {
public:
    // Block size the worker drains per popBlock + feeds to
    // TxChannel::process — must match TxChannel::kInSize=64
    // (the reference's getbuffsize(48000)).  Datagram-to-block
    // alignment is handled BY THE RING: mic samples arrive in
    // datagram-sized chunks (~9-10 at 48 k mic), accumulate,
    // and the worker drains them in fixed 64-sample blocks.
    static constexpr int kBlockSize = 64;

    // Construction does the wire-up: opens the WDSP TX channel,
    // installs `micSource`'s consumer (samples → ring), and
    // spawns the worker thread.  `wdsp` must be a loaded
    // WdspNative; `micSource` must outlive this object (caller
    // arranges teardown order so the mic source is destroyed
    // AFTER this worker).  If TxChannel::open fails, the worker
    // thread is NOT spawned and construction completes in an
    // inert state (the destructor still cleans up cleanly).
    TxDspWorker(WdspNative *wdsp, Hl2Ep6MicSource &micSource);

    // Tear-down sequence (matches the C reference's
    // destroy_cmbuffs / destroy_xmtr ordering):
    //   1. Stop new mic samples landing in the ring (clear the
    //      Hl2Ep6MicSource consumer).
    //   2. Signal the ring's shutdown + semaphore release so the
    //      worker's popBlock returns.
    //   3. Join the worker thread (bounded — the worker only
    //      blocks in popBlock and never on anything else).
    //   4. Close the WDSP TX channel.
    ~TxDspWorker();

    TxDspWorker(const TxDspWorker &)            = delete;
    TxDspWorker &operator=(const TxDspWorker &) = delete;

    // Operator-facing setter pass-throughs.  Each takes
    // TxChannel's channelMtx_ briefly via TxChannel's own setter
    // implementation — safe to call from the Qt main thread
    // while the worker is running.
    void setMode(TxChannel::Mode m)               { tx_.setMode(m); }
    void setBandpass(double opLowHz, double opHighHz) {
        tx_.setBandpass(opLowHz, opHighHz);
    }
    void setMicGainDb(double db)                  { tx_.setMicGainDb(db); }
    void setAlcMaxGainDb(double db)               { tx_.setAlcMaxGainDb(db); }
    void setLevelerOn(bool on, double topDb = 5.0) {
        tx_.setLevelerOn(on, topDb);
    }
    void setPhrotOn(bool on)                      { tx_.setPhrotOn(on); }

    // Component-6 hook.  Copies up to maxFrames complex<float>
    // samples of the LATEST TX I/Q block into `out`; returns the
    // number of frames copied (0 if no block has been produced
    // yet).  Takes latestMtx_ briefly — main-thread or wire-
    // thread can call this safely.  Component 6 (the EP2 packer)
    // will read this every wire cadence tick when it lands.
    int latestTxBlock(std::complex<float> *out, int maxFrames) const;

    // Diagnostics.
    long long blockCount()   const {
        return blockCount_.load(std::memory_order_relaxed);
    }
    long long errorCount()   const {
        return errorCount_.load(std::memory_order_relaxed);
    }
    long long overrunCount() const { return ring_.overrunCount(); }

private:
    void workerLoop();

    TxChannel        tx_;
    TxRing           ring_;
    Hl2Ep6MicSource &mic_;   // caller-owned; outlives this

    // Latest TX I/Q block — written by the worker after each
    // successful process() call, read by Component 6's EP2
    // packer when it lands.  Sized exactly kBlockSize.
    std::vector<std::complex<float>> latestBlock_;
    int                              latestBlockSize_ = 0;
    mutable std::mutex               latestMtx_;

    std::thread       worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<long long> blockCount_{0};
    std::atomic<long long> errorCount_{0};
};

} // namespace lyra::dsp
