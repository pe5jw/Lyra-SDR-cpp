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
#include <cstddef>
#include <mutex>
#include <semaphore>
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

    // ── Component 6: TX I/Q output (wire side) ──────────────────
    //
    // Datagram-sized handshake to the EP2 wire writer.  Matches the
    // verified C-source reference's outIQbufp + hsendIQSem +
    // hobbuffsRun[0] pattern (see docs/architecture/tx1_ssb_design.md
    // §5.7 for the verified cite + handshake semantics): a shared
    // 126-sample buffer + two binary semaphores giving strict
    // producer-consumer lockstep with NO accumulation queue between
    // WDSP TXA output and the wire.
    //
    // Reference flow (mapped):
    //   producer side:
    //     memcpy(outIQbufp, out, sizeof(complex) * 126)
    //     ReleaseSemaphore(hsendIQSem)        ← "data ready"
    //     WaitForSingleObject(hobbuffsRun[0]) ← BLOCK on consumed
    //   consumer side:
    //     WaitForMultipleObjects(...sem...)   ← wait for data
    //     pack to wire bytes
    //     ReleaseSemaphore(hobbuffsRun[0])    ← "consumed"
    //
    // Lyra deviation (documented + reasoned, per design rule 2):
    // the EP2 writer is on a hard 2.6 ms timer cadence (S2-locked
    // timer-paced writer) and CANNOT block — blocking would break
    // wire keepalive.  Consumer therefore uses NON-BLOCKING
    // try_acquire: if data ready, consume + release consumed; if
    // not, fall through to the mandatory zero-fill (which is
    // exactly what the reference does for !XmitBit anyway).
    // Sample-by-sample wire content is functionally identical to
    // the reference for both XmitBit states; only the consumer's
    // wait policy is adapted to Lyra's wire cadence guarantee.
    //
    // Wire-inert by construction: injectTxIq_ defaults FALSE.
    // While false, the producer NEVER calls signalAndWait() so
    // dataReady is never released → consumer always falls through
    // to zero-fill → no SSB I/Q ever lands on the wire.  Component
    // 6 plumbing ships safe; a follow-up commit wires the FSM
    // keydown/keyup to set/clear injectTxIq_ + start/stop the
    // WDSP TXA channel for the actual first SSB voice key-up.
    static constexpr int kEp2BlockSize = 126;  // samples per datagram

    // EP2 writer side — non-blocking try-acquire.  If data is ready
    // for this datagram: copies kEp2BlockSize complex<float> samples
    // into `out`, releases the consumed semaphore, returns true.  If
    // not: returns false (consumer zero-fills its wire buffer).
    bool tryConsumeTxIq(std::complex<float> *out) noexcept;

    // Operator-arm gate.  Defaults FALSE.  When false, the producer
    // NEVER signals dataReady → tryConsumeTxIq always returns false
    // → no SSB I/Q on the wire.  Component 7 wires this from
    // HL2Stream's FSM keydown/keyup (via the registered TxControl
    // callback) so producer and consumer flags flip in lockstep.
    void setInjectTxIq(bool on) noexcept {
        injectTxIq_.store(on, std::memory_order_release);
    }
    bool injectTxIq() const noexcept {
        return injectTxIq_.load(std::memory_order_acquire);
    }

    // ── Component 7: TX channel lifecycle pass-throughs ─────────
    //
    // FSM-callable wrappers around TxChannel::start() / ::stop().
    // start() = SetChannelState(ch, 1, 0) — non-blocking; arms the
    // WDSP TXA DSP thread for this channel.  stop() = SetChannelState
    // (ch, 0, 1) — BLOCKING flush (≤100ms for WDSP internal drain
    // of ALC + bandpass + other stateful stages).  HL2Stream's
    // registerTxControl callback wires these to the FSM keydown
    // (start at fsmKeydownPostMox) and keyup (stop at fsmKeyupFadeOut
    // — AFTER MoxEdgeFade reaches zero, per §5.7 keyup ordering
    // invariant).  Reference parity to SetChannelState(id(1,0), ...)
    // calls in the verified reference's chkMOX handler.
    void startTxChannel() { tx_.start(); }
    void stopTxChannel()  { tx_.stop();  }

    // Diagnostics.
    long long blockCount()   const {
        return blockCount_.load(std::memory_order_relaxed);
    }
    long long errorCount()   const {
        return errorCount_.load(std::memory_order_relaxed);
    }
    long long overrunCount() const { return ring_.overrunCount(); }
    // sip1 tap counter — bench instrument confirming the v0.3
    // PureSignal forward-compat ring is filling.  Counts complete
    // 126-sample blocks teed off the EP2 hand-off into the sip1
    // ring (which has NO consumer in v0.2; v0.3 calcc reads it).
    long long sip1FillCount() const {
        return sip1FillCount_.load(std::memory_order_relaxed);
    }
    // EP2-consumer underrun accounting lives on the HL2Stream
    // EP2-packer side (natural owner — underrun is the wire's
    // perspective).  HL2Stream tracks "SSB expected on this
    // datagram but tryConsumeTxIq returned false" → zero-fill +
    // counter increment.  See HL2Stream::txIqUnderruns.

private:
    void workerLoop();
    // Producer side of the EP2 hand-off — fills txIqBuf_,
    // releases dataReady, blocks on consumed.  Tees the buffer
    // into the sip1 ring on the way through.  Called only when
    // injectTxIq_ is true AND the accumulator has ≥ kEp2BlockSize
    // samples ready.
    void signalAndWaitForEp2Consumer() noexcept;

    TxChannel        tx_;
    TxRing           ring_;
    Hl2Ep6MicSource &mic_;   // caller-owned; outlives this

    std::thread       worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<long long> blockCount_{0};
    std::atomic<long long> errorCount_{0};

    // ── EP2 hand-off (reference-faithful 2-semaphore handshake) ──
    // Shared 126-sample buffer; written by the producer right
    // before releasing dataReady, read by the consumer right after
    // acquiring dataReady.  No protection needed beyond the
    // semaphore happens-before — only ONE party touches the buffer
    // at any moment by virtue of the handshake.
    std::vector<std::complex<float>> txIqBuf_;
    // Accumulator for fexchange0 64-sample blocks → 126-sample
    // wire buffer.  Producer-thread-only.  Carries 0-63 samples
    // over from the previous round-trip when 64+64=128 was packed
    // as 126+2.  Capacity 64+126 = 190 covers the worst case.
    std::vector<std::complex<float>> accum_;

    // Two binary semaphores, mirroring the reference's hsendIQSem
    // (data-ready) + hobbuffsRun[0] (consumed) pair.  std::
    // counting_semaphore<1> = binary semaphore in C++20.
    std::counting_semaphore<1> txIqDataReady_{0};
    std::counting_semaphore<1> txIqConsumed_{1};  // start "consumed"
                                                  // = buffer available
                                                  // for first fill

    std::atomic<bool>      injectTxIq_{false};

    // ── sip1 TX I/Q tap (v0.3 PureSignal forward-compat) ────────
    // Per design doc §5.8 / CLAUDE.md §7 (v0.2.0 mandatory): every
    // 126-sample EP2 hand-off is teed into this ring.  No consumer
    // in v0.2.0 — allocated + filled, that's it.  v0.3 PureSignal
    // calcc thread reads from it for adaptive predistortion
    // calibration.  Wiring now lets v0.3 land without re-validating
    // every TX sub-mode.  Size: 1 sec @ 48 kHz = 48000 complex
    // samples ≈ 384 KB.  SPSC; producer is the worker thread,
    // consumer (when v0.3 lands) will be the calcc thread.  No
    // backpressure: oldest data drops when ring wraps (PS
    // calibration only cares about RECENT samples anyway).
    static constexpr std::size_t kSip1RingSamples = 48000;
    std::vector<std::complex<float>> sip1Ring_;
    std::atomic<std::size_t>         sip1WriteIdx_{0};
    std::atomic<long long>           sip1FillCount_{0};
};

} // namespace lyra::dsp
