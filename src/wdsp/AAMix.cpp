// Lyra-cpp — AAMix.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream:    https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/aamix.c
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
//
// See AAMix.h for the full per-file GPL attribution + the
// C → C++23 idiom-translation log + the multi-stage port plan
// + the public-API contract notes ported verbatim from
// aamix.h:126-201.
//
// ---------------------------------------------------------------
//
// **Stage B.2 (THIS COMMIT) — constructor + destructor + slew
// helpers.**  Lands the bodies of create_aaslew /
// destroy_aaslew / flush_aaslew (reference aamix.c:69-125) +
// create_aamix (aamix.c:128-207) + destroy_aamix (aamix.c:209-234).
// Mixer is now constructable + destructible.  Still wire-inert
// (no caller invokes create_aamix yet — the cmaster pump wire-up
// lands in Stage D).
//
// **Deliberately deferred to Stage B.4**:
//   - mix_main thread entry (aamix.c:32-49)
//   - start_mixthread (aamix.c:51-55)
//   - close_mixer (aamix.c:471-491)
//   - open_mixer (aamix.c:493-505)
// The reference create_aamix calls start_mixthread on its last
// line (if nactive > 0); Stage B.2 replaces that single line with
// a TODO marker so the AAMix is fully initialized but its mix
// thread does NOT start.  Stage B.4 fills in the thread machinery.
// The reference destroy_aamix's stop-the-thread sequence
// (InterlockedBitTestAndReset(&run, 0) + per-input
// ReleaseSemaphore(Ready[i], 1, 0) + Sleep(2)) similarly defers
// to Stage B.4 — Stage B.2 destroy_aamix is a pure RAII cleanup
// (std::vector / std::mutex / unique_ptr<semaphore> auto-clean
// on AAMix's `delete`), correct because Stage B.2's create_aamix
// never started any thread.
//
// **Deliberately deferred to Stage B.3**:
//   - xMixAudio (per-input push)
//   - xaamix (mix-and-output pump)
//   - upslew / downslew (envelope state machines)
//   - flush_mix_ring (per-ring reset)
//
// **Deliberately deferred to Stage B.5**:
//   - 10 property setters (SetAAudioMix*, SetAAudioRing*,
//     SetAAudioOutRate, SetAAudioStreamRate)
//   - SetAAudioMixOutputPointer wire-up into the Stage A
//     CMaster::SendpOutboundRx stub

#include "wdsp/AAMix.h"
#include "wdsp_native.h"

#include <cmath>
#include <numbers>

namespace lyra::wdsp {

// Reference aamix.c:30 — `__declspec (align (16)) AAMIX
// paamix[MAX_EXT_AAMIX]`.  Process-lifetime central pointer bank.
// Stage B.2's create_aamix publishes a pointer into it (id >= 0);
// Stage B.2's destroy_aamix nulls the slot on teardown so a
// subsequent destroy_aamix(nullptr, id) is a safe no-op.
alignas(16) std::array<AAMix*, MAX_EXT_AAMIX> paamix{};

// =========================================================================
// Slew helpers — reference aamix.c:69-125.
// =========================================================================

// Reference aamix.c:69-107 — create_aaslew(a).
//
// Initializes the slew sub-struct's state machine + cosine
// envelope coefficient tables + semaphores + timeouts from
// the operator-supplied t{delay,slew}{up,down} timing.
//
// Cosine envelopes: aamix.c:84-98 builds `cup[]` as
// 0.5 * (1 - cos(theta)) over [0..ntup], and `cdown[]` as
// 0.5 * (1 + cos(theta)) over [0..ntdown].  Lyra-cpp uses
// std::numbers::pi_v<double> (C++20) for the reference's PI
// macro; coefficient values are byte-identical to the reference
// for the same input rates + times.
//
// Semaphores: max=1 / initial=0 matches CreateSemaphore(0, 0,
// 1, 0) at aamix.c:100-101.
//
// Timeouts: ms with 2 ms guard, identical to aamix.c:105-106.
static void create_aaslew(AAMix* a)
{
    a->slew.ustate = SlewState::BEGIN;
    a->slew.dstate = SlewState::BEGIN;
    a->slew.ucount = 0;
    a->slew.dcount = 0;
    a->slew.ndelup   = static_cast<int>(a->slew.tdelayup   * a->outrate);
    a->slew.ndeldown = static_cast<int>(a->slew.tdelaydown * a->outrate);
    a->slew.ntup     = static_cast<int>(a->slew.tslewup    * a->outrate);
    a->slew.ntdown   = static_cast<int>(a->slew.tslewdown  * a->outrate);
    a->slew.cup  .assign(static_cast<size_t>(a->slew.ntup   + 1), 0.0);
    a->slew.cdown.assign(static_cast<size_t>(a->slew.ntdown + 1), 0.0);

    {
        const double delta =
            std::numbers::pi_v<double> / static_cast<double>(a->slew.ntup);
        double theta = 0.0;
        for (int i = 0; i <= a->slew.ntup; ++i) {
            a->slew.cup[i] = 0.5 * (1.0 - std::cos(theta));
            theta += delta;
        }
    }
    {
        const double delta =
            std::numbers::pi_v<double> / static_cast<double>(a->slew.ntdown);
        double theta = 0.0;
        for (int i = 0; i <= a->slew.ntdown; ++i) {
            a->slew.cdown[i] = 0.5 * (1.0 + std::cos(theta));
            theta += delta;
        }
    }

    // Reference aamix.c:100-101 — CreateSemaphore(0, 0, 1, 0).
    // Translation: std::binary_semaphore (= counting_semaphore<1>)
    // with initial count 0.  unique_ptr because the semaphore has
    // no default ctor.
    a->slew.uwait = std::make_unique<std::binary_semaphore>(0);
    a->slew.dwait = std::make_unique<std::binary_semaphore>(0);

    // Reference aamix.c:102-103 — InterlockedBitTestAndReset
    // (&a->slew.uflag, 0).  bit 0 cleared.
    a->slew.uflag.store(0, std::memory_order_release);
    a->slew.dflag.store(0, std::memory_order_release);

    // Reference aamix.c:105-106.
    a->slew.utimeout =
        static_cast<int>(1000.0 * (a->slew.tdelayup   + a->slew.tslewup  )) + 2;
    a->slew.dtimeout =
        static_cast<int>(1000.0 * (a->slew.tdelaydown + a->slew.tslewdown)) + 2;
}

// Reference aamix.c:109-115 — destroy_aaslew(a).
//
// The reference uses CloseHandle(uwait/dwait) + _aligned_free
// (cup/cdown).  Lyra-cpp's RAII members (unique_ptr<semaphore>,
// std::vector<double>) handle teardown automatically when the
// containing AaSlew destructs.  This function is therefore a
// no-op in the Lyra-cpp port; retained as a documented hook in
// case a future change needs explicit slew teardown ordering vs
// other AAMix fields.
//
// **[Lyra-native — RAII deletion]**: zero-body.  No deviation
// from reference semantics; the reference's explicit CloseHandle
// + _aligned_free calls are subsumed by C++ destructor rules.
static void destroy_aaslew(AAMix* /*a*/)
{
    // intentionally empty — RAII drives teardown.
}

// Reference aamix.c:117-125 — flush_aaslew(a).
//
// Resets the slew state machine to BEGIN with zeroed counters
// + clears uflag/dflag.  Called from flush_mix_ring (Stage B.3).
// Public-via-Stage-B.3-callers; declared `static` until that
// caller lands.
//
// Lyra-cpp port: same semantics; atomic stores use release
// ordering so a subsequent xaamix thread observing the cleared
// flags sees the BEGIN state writes happens-before.
[[maybe_unused]] static void flush_aaslew(AAMix* a)
{
    a->slew.ustate = SlewState::BEGIN;
    a->slew.dstate = SlewState::BEGIN;
    a->slew.ucount = 0;
    a->slew.dcount = 0;
    a->slew.uflag.store(0, std::memory_order_release);
    a->slew.dflag.store(0, std::memory_order_release);
}

// =========================================================================
// create_aamix — reference aamix.c:128-207.
// =========================================================================
//
// Allocates an AAMix, copies operator-supplied parameters, sizes
// per-input ring buffers + resampler-output buffers, initializes
// per-input semaphores + mutexes + accept flags + Aready slot
// list, constructs per-input WDSP resamplers, initializes the
// slew sub-struct, sets the run bit, registers in paamix[id] if
// id >= 0.
//
// **Stage B.2 divergence (single line)** from reference:
// aamix.c:204 calls `start_mixthread(a)` if nactive > 0.  Stage
// B.2 replaces that call with a TODO marker.  Stage B.4 restores
// the thread launch; until then, the AAMix is fully populated
// but its mix thread does not run.  This is wire-inert per the
// Stage B plan (no consumer constructs AAMix until Stage D).
//
// Memory ownership: AAMix is heap-allocated via `new`.  Caller
// (cmaster pump in Stage D) must invoke destroy_aamix to delete.
// Mirrors the reference's malloc0/_aligned_free pattern.
AAMix* create_aamix(
    lyra::dsp::WdspNative& wdsp,
    int id,
    int outbound_id,
    int ringinsize,
    int outsize,
    int ninputs,
    long active,
    long what,
    double volume,
    int ring_size,
    int* inrates,
    int outrate,
    std::function<void(int id, int nsamples, double* buff)> Outbound,
    double tdelayup,
    double tslewup,
    double tdelaydown,
    double tslewdown)
{
    // Reference aamix.c:148 — `AAMIX a = (AAMIX) malloc0(...)`.
    // Lyra-cpp: heap-allocated + RAII-initialized via std::vector
    // / std::mutex / std::atomic in-class defaults.
    AAMix* a = new AAMix{};

    // [Lyra-native] DLL handle plumbing.  Single-field divergence
    // from reference (see AAMix.h `wdsp` field comment).
    a->wdsp = &wdsp;

    // Reference aamix.c:149-163 — operator-parameter copy.
    a->id          = id;
    a->outbound_id = outbound_id;
    a->ringinsize  = ringinsize;
    a->outsize     = outsize;
    a->ninputs     = ninputs;
    a->active.store(static_cast<std::uint32_t>(active),
                    std::memory_order_release);
    a->what.store(  static_cast<std::uint32_t>(what),
                    std::memory_order_release);
    a->volume      = volume;
    a->rsize       = ring_size;
    a->outrate     = outrate;
    a->Outbound    = std::move(Outbound);
    a->slew.tdelayup   = tdelayup;
    a->slew.tslewup    = tslewup;
    a->slew.tdelaydown = tdelaydown;
    a->slew.tslewdown  = tslewdown;

    // Reference aamix.c:164-165 — per-input ring buffers, sized
    // `rsize * sizeof(complex)` (2 doubles per complex sample).
    // std::vector zero-initialized via the (size, value) ctor.
    for (int i = 0; i < a->ninputs; ++i) {
        a->ring[i].assign(static_cast<size_t>(2 * a->rsize), 0.0);
    }
    // Reference aamix.c:166 — output buffer (interleaved IQ).
    a->out.assign(static_cast<size_t>(2 * a->outsize), 0.0);

    // Reference aamix.c:167 — initial nactive=0; rebuilt in the
    // active-mask loop below.
    a->nactive = 0;

    // Reference aamix.c:168-184 — per-input state + semaphore +
    // critical section + active-mask membership.
    const std::uint32_t active_mask = a->active.load(std::memory_order_acquire);
    for (int i = 0; i < a->ninputs; ++i) {
        a->vol[i]            = 1.0;
        a->tvol[i]           = a->volume;
        a->inidx[i]          = 0;
        a->outidx[i]         = 0;
        a->unqueuedsamps[i]  = 0;

        // Reference aamix.c:175 — CreateSemaphore(0, 0, 1000, 0).
        // max=1000 matches std::counting_semaphore<1000>; initial
        // count 0; unique_ptr because counting_semaphore has no
        // default ctor.
        a->Ready[i] = std::make_unique<std::counting_semaphore<1000>>(0);

        // Reference aamix.c:176 — InitializeCriticalSectionAndSpinCount
        // (&a->cs_in[i], 2500).  std::mutex (default-constructed
        // via in-class init); spin-count discarded — std::mutex
        // implementations spin briefly before kernel wait anyway,
        // so the contention profile is equivalent.

        if (active_mask & (1u << i)) {
            // Active input: register the Ready semaphore in
            // aready[] (the AND-wait list mix_main iterates), bump
            // nactive, and set the accept flag bit 0.
            a->aready[a->nactive++] = a->Ready[i].get();
            a->accept[i].fetch_or(1u, std::memory_order_release);
        } else {
            // Inactive input: clear accept bit 0.
            a->accept[i].fetch_and(~1u, std::memory_order_release);
        }
    }

    // Reference aamix.c:185-198 — per-input resampler construction.
    // The reference comment at :189 is load-bearing: "inrate &
    // outrate must be related by an integer multiple or sub-
    // multiple".  size is the resampler's per-call buffer length
    // (in input samples), scaled from ringinsize by the rate
    // ratio.  run=1 when rates differ, run=0 when they match
    // (no-op pass-through inside the WDSP resampler).
    const lyra::dsp::WdspApi& api = wdsp.api();
    for (int i = 0; i < a->ninputs; ++i) {
        int run;
        int size;
        a->inrate[i] = inrates[i];
        if (a->inrate[i] != a->outrate) {
            run = 1;
        } else {
            run = 0;
        }
        if (a->inrate[i] > a->outrate) {
            size = a->ringinsize * (a->inrate[i] / a->outrate);
        } else {
            size = a->ringinsize / (a->outrate / a->inrate[i]);
        }

        // Reference aamix.c:196 — resampler-output buffer sized
        // ringinsize * sizeof(complex).
        a->resampbuff[i].assign(static_cast<size_t>(2 * a->ringinsize), 0.0);

        // Reference aamix.c:197 — create_resample(run, size, in=0,
        // out=resampbuff, in_rate, out_rate, fc=0.0, ncoef=0,
        // gain=1.0).  in=nullptr is set per-push by xMixAudio
        // (Stage B.3) before each xresample call.
        a->rsmp[i] = api.create_resample(
            run, size, nullptr, a->resampbuff[i].data(),
            a->inrate[i], a->outrate, 0.0, 0, 1.0);
    }

    // Reference aamix.c:199 — cs_out spin-count 2500 → default
    // std::mutex (in-class init, no setup call needed).

    // Reference aamix.c:201 — slew construction.
    create_aaslew(a);

    // Reference aamix.c:203 — InterlockedBitTestAndSet(&run, 0).
    a->run.fetch_or(1u, std::memory_order_release);

    // Reference aamix.c:204 — `if (a->nactive) start_mixthread(a)`.
    // Stage B.2 defers thread launch to Stage B.4.  The AAMix is
    // fully populated; mix_main does not run.
    //
    // STAGE B.4 PENDING: if (a->nactive > 0) start_mixthread(a);

    // Reference aamix.c:205 — paamix[id] publication.
    if (a->id >= 0 && a->id < MAX_EXT_AAMIX) {
        paamix[a->id] = a;
    }

    return a;
}

// =========================================================================
// destroy_aamix — reference aamix.c:209-234.
// =========================================================================
//
// **Stage B.2 simplification**: the reference destroy_aamix
// performs a stop-the-thread dance (clear run bit, ReleaseSemaphore
// on every Ready[i] to wake mix_main, Sleep(2) to give the thread
// time to exit, then DeleteCriticalSection / destroy_resample /
// _aligned_free in reverse construction order).  Stage B.2's
// create_aamix does NOT start mix_main (Stage B.4 territory), so
// there is no thread to stop.  Stage B.2 destroy_aamix:
//
//   1. Looks up the AAMix via paamix[id] if ptr is nullptr.
//   2. Nulls the paamix slot so subsequent destroy(nullptr, id)
//      is a safe no-op.
//   3. Destroys per-input WDSP resamplers via the DLL handle.
//   4. Deletes the AAMix object — RAII (std::vector / std::mutex /
//      std::unique_ptr<semaphore>) handles the rest.
//
// When Stage B.4 lands, the run-bit-clear + ReleaseSemaphore-wake
// + thread-join sequence is added back before steps 3-4.
void destroy_aamix(AAMix* ptr, int id)
{
    AAMix* a = nullptr;
    if (ptr == nullptr) {
        if (id >= 0 && id < MAX_EXT_AAMIX) {
            a = paamix[id];
            paamix[id] = nullptr;
        }
    } else {
        a = ptr;
        // Also clear the central-bank slot if this AAMix was
        // registered there (id >= 0 ⇒ create_aamix published it).
        if (a->id >= 0 && a->id < MAX_EXT_AAMIX &&
            paamix[a->id] == a) {
            paamix[a->id] = nullptr;
        }
    }
    if (a == nullptr) return;

    // STAGE B.4 PENDING (reference aamix.c:215-218):
    //   a->run.fetch_and(~1u, std::memory_order_release);
    //   for (int i = 0; i < a->ninputs; ++i)
    //       a->Ready[i]->release();  // wake mix_main
    //   std::this_thread::sleep_for(std::chrono::milliseconds(2));
    //   // (mix_thread.request_stop() + join via jthread RAII)
    //
    // At Stage B.2 the thread never started; skipping is safe.

    // Reference aamix.c:219-226 — destroy per-input WDSP resamplers.
    // RAII auto-cleans std::vector resampbuff[]; std::mutex cs_in[];
    // std::counting_semaphore Ready[].
    if (a->wdsp != nullptr) {
        const lyra::dsp::WdspApi& api = a->wdsp->api();
        for (int i = 0; i < a->ninputs; ++i) {
            if (a->rsmp[i] != nullptr && api.destroy_resample != nullptr) {
                api.destroy_resample(a->rsmp[i]);
                a->rsmp[i] = nullptr;
            }
        }
    }

    // Reference aamix.c:230-232 — destroy_aaslew + _aligned_free(a).
    destroy_aaslew(a);  // no-op (RAII)
    delete a;
}

} // namespace lyra::wdsp
