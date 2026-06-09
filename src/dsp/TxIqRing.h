// Lyra-cpp — TxIqRing.h
//
// Lyra-native SPSC ring decoupling the TX-DSP producer (TXA chain's
// Outbound, called on the Hl2Ep6MicSource rx-loop thread) from the
// HL2Stream EP2 wire-writer consumer (timer-paced at ~380 Hz pulling
// via the registered TxIqSource closure).
//
// **Stage 7.1** of the TX-side wire-consumer rebuild (Step 14
// THETIS_DIRECT_PORT_PLAN.md / docs/architecture/STAGE_7_TX_WIRE_DESIGN.md
// §11.8 v2 commit table).
//
// ── Why Lyra-native, not ported ──────────────────────────────────
//
// The openHPSDR Thetis reference handles this same decouple via
// ChannelMaster's `cmbuffs.c` CMB ring + Inbound()/Outbound()
// semaphore handshake — a C-era mechanism tightly coupled to the
// reference's pump-thread + per-stream-semaphore model.  Lyra-cpp
// drives the TX-DSP chain directly from the Ep6 rx-loop thread (per
// the Stage 7 plan-paper §11.3 + the parent project's MEMORY
// §15.7/§15.26 wire-clock-master discipline), so the CMB pump
// thread + cmbuffs surface aren't needed.  The Lyra-native answer
// is a small fixed-capacity SPSC ring of `std::complex<float>` (the
// HL2Stream::TxIqSource pull contract's element type, so the
// `double → float` conversion happens ONCE at push time, never on
// the wire-writer's hot path).
//
// Drop-oldest discipline matches the §15.7/§15.26 wire-clock-master
// principle: the WIRE is the time base; under burst arrival the
// FRESHEST mic samples reach the wire, stale samples are discarded.
// This is the OPPOSITE of a blocking back-pressure semaphore (which
// would couple the rx-loop to the wire writer and reintroduce the
// GIL-style cadence-quantisation the parent project spent multi-
// week §15.7/§15.26 effort eliminating).
//
// ── Threading contract ───────────────────────────────────────────
//
// Strict SPSC at the API level:
//   * pushFromInterleavedDoubles() — single producer thread (the
//     Ep6 rx-loop thread inside the SendpOutboundTx lambda).
//   * popBlock126()                 — single consumer thread (the
//     HL2Stream EP2 writer thread inside the registered
//     TxIqSource closure).
//
// Implementation uses one small std::mutex.  At ~380 Hz wire
// cadence + a few-kHz mic block rate, the critical section is
// dozens of ns — uncontested in practice.  A genuinely lock-free
// SPSC with drop-oldest is materially more complex (atomic
// head/tail with the producer ALSO advancing tail past the
// consumer's read position requires a CAS / seqlock dance) without
// observable benefit at this rate; the parent project's MEMORY
// §15.26 ATT-on-TX RA-1 deadlock-stress proof exists precisely
// because the std::mutex SPSC pattern is so well-trodden.
//
// Lock-ordering invariant (per the same MEMORY discipline):
//   * push and pop ONLY take `mu_`.  Neither nests another lock
//     INSIDE `mu_`.
//   * The producer-side SendpOutboundTx lambda + the consumer-side
//     TxIqSource closure are the ONLY callers; both are leaf
//     callees on their respective threads (no outer lock held).
//
// ── B.6.b-class landmine guard ───────────────────────────────────
//
// The ring is owned by Radio as `std::shared_ptr<TxIqRing>`; both
// producer + consumer closures capture `std::weak_ptr<TxIqRing>`
// and `.lock()` on every call.  Radio::stop() teardown ordering
// (per STAGE_7_TX_WIRE_DESIGN.md §11.5) ensures the closures are
// CLEARED (`SendpOutboundTx({})` + `registerTxIqSource({})`)
// BEFORE the shared_ptr drops, so a late call against a torn-down
// ring is impossible.  The weak_ptr is the second-line guard.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace lyra::dsp {

// Fixed-capacity SPSC ring of std::complex<float> samples with
// drop-oldest overflow discipline.  See file-level comment for the
// full threading + ownership contract.
class TxIqRing final {
public:
    // The wire-side TxIqSource pull contract delivers exactly 126
    // complex samples per EP2 datagram (one HPSDR P1 USB frame's
    // worth of TX I/Q at 48 kHz / 2.625 ms).  Matches the EP2
    // packer body at hl2_stream.cpp:2468 `std::complex<float>
    // ssbBuf[126];`.
    static constexpr int kBlockSize = 126;

    // capacity_samples is the maximum number of std::complex<float>
    // samples the ring can hold.  A typical choice is 512 (~336 ms
    // cushion at 380 Hz / ~10.7 ms at 48 kHz), giving plenty of
    // headroom for a transient Qt-main-thread / GIL-class stall on
    // the rx-loop thread without starving the wire.  Construction
    // throws std::bad_alloc on allocation failure; capacity_samples
    // == 0 is clamped to 1.
    explicit TxIqRing(std::size_t capacity_samples);

    TxIqRing(const TxIqRing&) = delete;
    TxIqRing& operator=(const TxIqRing&) = delete;
    TxIqRing(TxIqRing&&) = delete;
    TxIqRing& operator=(TxIqRing&&) = delete;
    ~TxIqRing() = default;

    // Producer-side push.  iq_pairs points to n_samples * 2 doubles
    // in interleaved {I, Q, I, Q, ...} order (the WDSP TXA chain's
    // Outbound format).  Conversion to std::complex<float> happens
    // here, once.
    //
    // Drop-oldest overflow: if the ring lacks room for the incoming
    // batch, the OLDEST samples currently held are discarded to
    // make space.  If n_samples itself exceeds capacity, the
    // LEADING n_samples-capacity samples of the incoming batch are
    // also discarded; only the trailing capacity samples are kept
    // (the freshest), matching the wire-clock-master discipline.
    //
    // Returns the number of samples dropped due to overflow during
    // this call (0 in steady state).  Diagnostic only; callers may
    // ignore.  No-op + returns 0 if n_samples <= 0 or iq_pairs is
    // null.  noexcept; never throws.
    std::size_t pushFromInterleavedDoubles(const double* iq_pairs,
                                            int n_samples) noexcept;

    // Consumer-side pop.  Writes exactly kBlockSize (126)
    // std::complex<float> samples to out and returns true iff a
    // full block was available.  Non-blocking — the wire-writer
    // cannot afford to wait on a hard 2.625 ms timer cadence; an
    // underrun returns false and the EP2 packer's existing
    // zero-fill fall-through path runs (see hl2_stream.cpp:2480-
    // 2488).  No-op + returns false if out is null.
    bool popBlock126(std::complex<float>* out) noexcept;

    // Diagnostics.  All thread-safe (take mu_), all noexcept.
    std::size_t size() const noexcept;        // current sample count
    std::size_t capacity() const noexcept { return capacity_; }
    std::uint64_t totalDropped() const noexcept;  // lifetime overflow
    void clear() noexcept;                    // drop all, reset

private:
    mutable std::mutex mu_;
    std::vector<std::complex<float>> buf_;
    std::size_t capacity_;     // == buf_.size(); cached for hot path
    std::size_t head_ = 0;     // next-write index
    std::size_t tail_ = 0;     // next-read index
    std::size_t count_ = 0;    // current sample count (0..capacity_)
    std::uint64_t totalDropped_ = 0;
};

}  // namespace lyra::dsp
