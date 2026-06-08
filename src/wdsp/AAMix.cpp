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
// **Stage B.3 (THIS COMMIT) — DSP hot path.**  Adds the bodies
// of xMixAudio (aamix.c:237-278) + upslew (aamix.c:280-343) +
// downslew (aamix.c:345-420) + xaamix (aamix.c:423-459) +
// flush_mix_ring (aamix.c:461-469).  Mixer can now mix.  Still
// wire-inert (no consumer constructs the AAMix or pushes samples
// to it — the cmaster pump wire-up lands in Stage D).
//
// **Already shipped in Stage B.2**: create_aaslew, destroy_aaslew,
// flush_aaslew, create_aamix, destroy_aamix.
//
// **Deliberately deferred to Stage B.4**:
//   - mix_main thread entry (aamix.c:32-49)
//   - start_mixthread (aamix.c:51-55)
//   - close_mixer (aamix.c:471-491)
//   - open_mixer (aamix.c:493-505)
//
// **Deliberately deferred to Stage B.5**:
//   - 10 property setters (SetAAudioMix*, SetAAudioRing*,
//     SetAAudioOutRate, SetAAudioStreamRate)
//   - SetAAudioMixOutputPointer wire-up into the Stage A
//     CMaster::SendpOutboundRx stub

#include "wdsp/AAMix.h"
#include "wdsp_native.h"

#include <chrono>
#include <cmath>
#include <cstring>
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

// =========================================================================
// DSP hot path — reference aamix.c:237-469.
// =========================================================================

// Forward declarations for the upslew/downslew helpers (file-internal
// state machines called only from xaamix).
static void upslew  (AAMix* a);
static void downslew(AAMix* a);

// Reference aamix.c:237-278 — xMixAudio(ptr, id, stream, data).
//
// Per-input sample push entry point.  Called by upstream pump
// (Stage D cmaster) for every block of input samples destined for
// this mixer's `stream`.  Resamples (if rates differ) into the
// per-stream ring buffer; releases the per-stream Ready semaphore
// when ≥outsize complex samples have accumulated since the last
// release; advances the wrap-around write index.
//
// Locking: cs_in[stream] serialises with concurrent xMixAudio
// calls on the same stream (very unlikely in practice — one
// upstream producer per stream — but the reference takes the lock
// unconditionally and so does the port).  cs_out is NOT taken
// here (the resampler-output buffer and ring are per-stream so
// xaamix's mix-and-output path does not conflict with the ring
// write itself; xaamix only reads `outidx` and `tvol`).
//
// `data` is interleaved complex (I,Q,I,Q,...), length = a's
// configured input-rate-equivalent of ringinsize complex samples
// (i.e. `2 * size_passed_to_create_resample` doubles when the
// resampler runs; `2 * ringinsize` doubles when it doesn't).
void xMixAudio(AAMix* ptr, int id, int stream, double* data)
{
    AAMix* a = (ptr == nullptr) ? paamix[id] : ptr;
    if (a == nullptr) return;

    // Reference aamix.c:244 — accept bit 0 gate (input is open
    // for incoming samples).  Acquire ordering pairs with the
    // release in create_aamix / SetAAudioMixState{,s} /
    // open_mixer.
    if ((a->accept[stream].load(std::memory_order_acquire) & 1u) == 0u) {
        return;
    }

    // Reference aamix.c:246 — EnterCriticalSection(&cs_in[stream]).
    std::scoped_lock lock(a->cs_in[stream]);

    // Reference aamix.c:247-254 — optional resample.  Direct
    // field access on the WDSP resample struct (`->in`/`->out`/
    // `->run`) per the upstream aamix.c pattern; the public
    // struct layout is replicated in Resample.h with full
    // attribution so this static_cast is well-defined.
    double* indata = nullptr;
    resample* rsmp = static_cast<resample*>(a->rsmp[stream]);
    if (rsmp != nullptr && rsmp->run) {
        rsmp->in = data;
        indata   = a->resampbuff[stream].data();
        if (a->wdsp != nullptr) {
            a->wdsp->api().xresample(rsmp);
        }
    } else {
        indata = data;
    }

    // Reference aamix.c:255-264 — wrap-around ring write.  The
    // ring holds `rsize` complex samples (== 2*rsize doubles);
    // inidx is the *complex* write index.
    int first  = 0;
    int second = 0;
    if (a->ringinsize > (a->rsize - a->inidx[stream])) {
        first  = a->rsize - a->inidx[stream];
        second = a->ringinsize - first;
    } else {
        first  = a->ringinsize;
        second = 0;
    }
    // memcpy of `first` complex samples (2 doubles each) into the
    // ring starting at the current inidx position.
    std::memcpy(a->ring[stream].data() + 2 * a->inidx[stream],
                indata,
                static_cast<size_t>(first) * 2 * sizeof(double));
    // Wrap-around tail: `second` complex samples from offset
    // `2 * first` of the indata into the start of the ring.
    if (second > 0) {
        std::memcpy(a->ring[stream].data(),
                    indata + 2 * first,
                    static_cast<size_t>(second) * 2 * sizeof(double));
    }

    // Reference aamix.c:268-273 — semaphore-release accounting.
    // unqueuedsamps tracks samples written but not yet "released"
    // to the mixer thread; once it crosses outsize, release the
    // Ready semaphore `n` times so the mixer can dequeue `n`
    // output frames' worth of samples.
    a->unqueuedsamps[stream] += a->ringinsize;
    if (a->unqueuedsamps[stream] >= a->outsize) {
        const int n = a->unqueuedsamps[stream] / a->outsize;
        // Reference aamix.c:271 — ReleaseSemaphore(Ready[stream], n, 0).
        // std::counting_semaphore::release(n) bumps the permit
        // count by n in one call (C++20).  Matches semantics.
        if (a->Ready[stream]) a->Ready[stream]->release(n);
        a->unqueuedsamps[stream] -= n * a->outsize;
    }

    // Reference aamix.c:274-275 — advance inidx with wrap.
    a->inidx[stream] += a->ringinsize;
    if (a->inidx[stream] >= a->rsize) {
        a->inidx[stream] -= a->rsize;
    }
    // cs_in[stream] released by scoped_lock dtor.
}

// Reference aamix.c:280-343 — upslew(a).
//
// Cosine-shaped fade-in state machine called from xaamix when
// slew.uflag is set.  Iterates the freshly-mixed output buffer
// (a->out) sample-by-sample and applies the BEGIN → DELAYUP →
// UPSLEW → ON envelope transitions.  When the ON state is
// reached AND the iteration finishes a buffer, ON resets to
// BEGIN, clears uflag, and releases the uwait semaphore so the
// open_mixer caller (Stage B.4) unblocks.
//
// Ported verbatim from aamix.c:280-343 — the switch-on-ustate
// dispatch + the per-state pout assignments + state-transition
// arithmetic match reference byte-for-byte.  Only translation:
// SlewState enum class vs int; uflag atomic write uses release
// ordering.
static void upslew(AAMix* a)
{
    double* pin  = a->out.data();
    double* pout = a->out.data();
    for (int i = 0; i < a->outsize; ++i) {
        const double I = pin[2 * i + 0];
        const double Q = pin[2 * i + 1];
        switch (a->slew.ustate) {
        case SlewState::BEGIN:
            pout[2 * i + 0] = 0.0;
            pout[2 * i + 1] = 0.0;
            if ((I != 0.0) || (Q != 0.0)) {
                if (a->slew.ndelup > 0) {
                    a->slew.ustate = SlewState::DELAYUP;
                    a->slew.ucount = a->slew.ndelup;
                } else if (a->slew.ntup > 0) {
                    a->slew.ustate = SlewState::UPSLEW;
                    a->slew.ucount = a->slew.ntup;
                } else {
                    a->slew.ustate = SlewState::ON;
                }
            }
            break;
        case SlewState::DELAYUP:
            pout[2 * i + 0] = 0.0;
            pout[2 * i + 1] = 0.0;
            if (a->slew.ucount-- == 0) {
                if (a->slew.ntup > 0) {
                    a->slew.ustate = SlewState::UPSLEW;
                    a->slew.ucount = a->slew.ntup;
                } else {
                    a->slew.ustate = SlewState::ON;
                }
            }
            break;
        case SlewState::UPSLEW:
            pout[2 * i + 0] = I * a->slew.cup[a->slew.ntup - a->slew.ucount];
            pout[2 * i + 1] = Q * a->slew.cup[a->slew.ntup - a->slew.ucount];
            if (a->slew.ucount-- == 0) {
                a->slew.ustate = SlewState::ON;
            }
            break;
        case SlewState::ON:
            pout[2 * i + 0] = I;
            pout[2 * i + 1] = Q;
            if (i == a->outsize - 1) {
                a->slew.ustate = SlewState::BEGIN;
                a->slew.uflag.fetch_and(~1u, std::memory_order_release);
                if (a->slew.uwait) a->slew.uwait->release();
            }
            break;
        default:
            break;
        }
    }
}

// Reference aamix.c:345-420 — downslew(a).
//
// Cosine-shaped fade-out state machine, mirror of upslew.
// Iterates a->out and applies BEGIN → DELAYDOWN → DOWNSLEW →
// ZERO → OFF envelope transitions.  In OFF state on iteration
// completion, resets to BEGIN, clears dflag, releases dwait so
// the close_mixer caller (Stage B.4) unblocks.
//
// Ported verbatim from reference; same translation list as
// upslew above.
static void downslew(AAMix* a)
{
    double* pin  = a->out.data();
    double* pout = a->out.data();
    for (int i = 0; i < a->outsize; ++i) {
        const double I = pin[2 * i + 0];
        const double Q = pin[2 * i + 1];
        switch (a->slew.dstate) {
        case SlewState::BEGIN:
            pout[2 * i + 0] = I;
            pout[2 * i + 1] = Q;
            if (a->slew.ndeldown > 0) {
                a->slew.dstate = SlewState::DELAYDOWN;
                a->slew.dcount = a->slew.ndeldown;
            } else if (a->slew.ntdown > 0) {
                a->slew.dstate = SlewState::DOWNSLEW;
                a->slew.dcount = a->slew.ntdown;
            } else {
                a->slew.dstate = SlewState::ZERO;
                a->slew.dcount = a->outsize;
            }
            break;
        case SlewState::DELAYDOWN:
            pout[2 * i + 0] = I;
            pout[2 * i + 1] = Q;
            if (a->slew.dcount-- == 0) {
                if (a->slew.ntdown > 0) {
                    a->slew.dstate = SlewState::DOWNSLEW;
                    a->slew.dcount = a->slew.ntdown;
                } else {
                    a->slew.dstate = SlewState::ZERO;
                    a->slew.dcount = a->outsize;
                }
            }
            break;
        case SlewState::DOWNSLEW:
            pout[2 * i + 0] =
                I * a->slew.cdown[a->slew.ntdown - a->slew.dcount];
            pout[2 * i + 1] =
                Q * a->slew.cdown[a->slew.ntdown - a->slew.dcount];
            if (a->slew.dcount-- == 0) {
                a->slew.dstate = SlewState::ZERO;
                a->slew.dcount = a->outsize;
            }
            break;
        case SlewState::ZERO:
            pout[2 * i + 0] = 0.0;
            pout[2 * i + 1] = 0.0;
            if (a->slew.dcount-- == 0) {
                a->slew.dstate = SlewState::OFF;
            }
            break;
        case SlewState::OFF:
            pout[2 * i + 0] = 0.0;
            pout[2 * i + 1] = 0.0;
            if (i == a->outsize - 1) {
                a->slew.dstate = SlewState::BEGIN;
                a->slew.dflag.fetch_and(~1u, std::memory_order_release);
                if (a->slew.dwait) a->slew.dwait->release();
            }
            break;
        default:
            break;
        }
    }
}

// Reference aamix.c:423-459 — xaamix(a).
//
// Mixer pump body called from mix_main (Stage B.4) after the
// AND-wait completes.  Steps:
//   1. Take cs_out.
//   2. Re-check `run`; if cleared, return (mix_main will see the
//      cleared bit on its next while-check and exit).  The
//      reference calls `_endthread()` here — Lyra-cpp uses a
//      plain `return` because cleanup is RAII + jthread join
//      (Stage B.4); the operator-visible "thread exits soon
//      after run is cleared" semantic is preserved.
//   3. Zero a->out.
//   4. Compute mix-mask = what & active (snapshot atomically).
//   5. For each set bit, sum the per-input ring contents
//      (starting at outidx[i], advancing per output sample)
//      times tvol[i] into a->out.
//   6. For each accept-bit-set input, advance outidx[i] by
//      outsize (with wrap).
//   7. If uflag set, run upslew over a->out.
//   8. If dflag set, run downslew over a->out.
//   9. Release cs_out (scoped_lock dtor).
void xaamix(AAMix* a)
{
    std::scoped_lock lock(a->cs_out);

    // Reference aamix.c:428-433 — early exit if run cleared.
    if ((a->run.load(std::memory_order_acquire) & 1u) == 0u) {
        // [Lyra-native — _endthread() → return]: the reference
        // calls _endthread() inside the locked section + then
        // falls through to LeaveCriticalSection and return.
        // Lyra-cpp's scoped_lock dtor releases cs_out
        // automatically on return, and the mix_main loop's
        // `run` check on the next iteration handles thread
        // exit (RAII jthread join — Stage B.4).
        return;
    }

    // Reference aamix.c:434 — `memset (a->out, 0, outsize *
    // sizeof(complex))`.
    std::fill(a->out.begin(), a->out.end(), 0.0);

    // Reference aamix.c:435 — snapshot what & active into a
    // local mask the loop walks.  Both reads use acquire
    // ordering to pair with the producer-side release writes
    // in SetAAudioMixState{,s} / SetAAudioMixWhat (Stage B.5).
    std::uint32_t what = a->what.load(std::memory_order_acquire) &
                         a->active.load(std::memory_order_acquire);

    // Reference aamix.c:436-452 — per-input mix loop.  Walks set
    // bits; for each one, sums `outsize` complex samples from
    // ring[i] starting at outidx[i] (with wrap on rsize) into
    // a->out, scaled by tvol[i].
    int i = 0;
    while (what != 0u) {
        const std::uint32_t mask = 1u << i;
        if ((mask & what) != 0u) {
            int idx = a->outidx[i];
            const double tv = a->tvol[i];
            for (int j = 0; j < a->outsize; ++j) {
                a->out[2 * j + 0] += tv * a->ring[i][2 * idx + 0];
                a->out[2 * j + 1] += tv * a->ring[i][2 * idx + 1];
                if (++idx == a->rsize) idx = 0;
            }
            what &= ~mask;
        }
        ++i;
    }

    // Reference aamix.c:453-455 — advance outidx for every input
    // whose accept bit is set (NOT just the mixed inputs — every
    // accepting input's ring is consumed in lockstep so they stay
    // aligned for future mix windows).
    for (int k = 0; k < a->ninputs; ++k) {
        if ((a->accept[k].load(std::memory_order_acquire) & 1u) != 0u) {
            a->outidx[k] += a->outsize;
            if (a->outidx[k] >= a->rsize) a->outidx[k] -= a->rsize;
        }
    }

    // Reference aamix.c:456-457 — slew dispatch.
    if ((a->slew.uflag.load(std::memory_order_acquire) & 1u) != 0u) {
        upslew(a);
    }
    if ((a->slew.dflag.load(std::memory_order_acquire) & 1u) != 0u) {
        downslew(a);
    }
    // cs_out released by scoped_lock dtor.
}

// Reference aamix.c:461-469 — flush_mix_ring(a, stream).
//
// Zeros the per-stream ring buffer, resets indices, drains any
// pending Ready semaphore permits, and flushes the WDSP
// resampler's internal state.  Called from close_mixer
// (Stage B.4) for every input on mixer close.  Public to
// in-file callers in B.4; declared `static` until then.
//
// The reference `while (!WaitForSingleObject(Ready[stream], 1))`
// loop is a polled-drain — WFSO returns 0 (WAIT_OBJECT_0) if a
// permit was acquired (loop continues), non-zero on 1 ms
// timeout (loop exits).  Lyra-cpp uses try_acquire_for(1 ms)
// which returns true on acquire / false on timeout — same
// semantics, inverted boolean.
[[maybe_unused]] static void flush_mix_ring(AAMix* a, int stream)
{
    // Reference aamix.c:463 — memset ring (size = rsize complex
    // = 2*rsize doubles).
    std::fill(a->ring[stream].begin(), a->ring[stream].end(), 0.0);
    a->inidx[stream]         = 0;
    a->outidx[stream]        = 0;
    a->unqueuedsamps[stream] = 0;
    // Reference aamix.c:467 — drain Ready semaphore until empty.
    if (a->Ready[stream]) {
        while (a->Ready[stream]->try_acquire_for(
                   std::chrono::milliseconds(1))) {
            // intentionally empty
        }
    }
    // Reference aamix.c:468 — flush the WDSP resampler.
    if (a->wdsp != nullptr && a->rsmp[stream] != nullptr &&
        a->wdsp->api().flush_resample != nullptr) {
        a->wdsp->api().flush_resample(a->rsmp[stream]);
    }
}

} // namespace lyra::wdsp
