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
// **Stage B.5 (THIS COMMIT) — 10 property setters + CMaster
// wire-up.**  Adds the bodies of SetAAudioMixOutputPointer,
// SetAAudioMixState{,s}, SetAAudioMixWhat, SetAAudioMixVolume,
// SetAAudioMixVol, SetAAudioRing{In,Out}size, SetAAudioOutRate,
// SetAAudioStreamRate (aamix.c:513-699).  Most reach
// close_mixer/open_mixer from Stage B.4 to safely re-edit the
// mixer state while data may be flowing; the volume/vol/what
// setters mutate fields directly under cs_out (they don't
// require a slewed shutdown).
//
// **Plus**: replaces the `// Stage B PENDING:
// SetAAudioMixOutputPointer(0, 0, pcm->OutboundRx);` comment
// in src/wire/CMaster.cpp's Stage A SendpOutboundRx stub with
// the actual call.  This is the architectural hand-off Stage A
// wired the storage slot for: SendpOutboundRx still stores the
// callback into pcm->OutboundRx (the central CMasterState
// global), AND now also pushes it into AAMix's central
// pointer-bank slot 0 so any future code constructing the
// AAMix RX mixer at id=0 picks up the same operator-registered
// callback.
//
// **Already shipped in Stages B.2-B.4**: create_aaslew,
// destroy_aaslew, flush_aaslew, create_aamix, destroy_aamix
// (B.2); xMixAudio, upslew, downslew, xaamix, flush_mix_ring
// (B.3); mix_main, start_mixthread, close_mixer, open_mixer
// (B.4).
//
// **Stage B.6 (DEFERRED — separate later commit)**: migrate
// the existing Lyra-native audio_mixer to use the ported AAMix
// in the RX audio routing path.  THAT commit is the
// wire-effective migration where the operator HL2 bench-gate
// matters; Stage B.5 finishes the ported library + the
// architectural hand-off but no Lyra consumer constructs an
// AAMix yet.
//
// (Historical Stage B.4 docstring — superseded by the above.)
// Adds mix_main (aamix.c:32-49) + start_mixthread (aamix.c:51-55)
// + close_mixer (aamix.c:471-491) + open_mixer (aamix.c:493-505)
// + restores the `start_mixthread(a)` call in create_aamix that
// Stage B.2 deferred + adds the thread-stop sequence to
// destroy_aamix that Stage B.2 deferred.  Mixer is now fully
// self-driving: mix_main wakes on the AND-wait, calls xaamix,
// invokes the Outbound callback, loops.  open_mixer / close_mixer
// orchestrate the slewed-shutdown / slewed-startup cycle that
// SetAAudioMixState{,s} use to add/remove streams cleanly.
// Still wire-inert (no consumer constructs the AAMix yet; the
// cmaster pump wire-up lands in Stage D).
//
// **Already shipped in Stages B.2-B.3**: create_aaslew,
// destroy_aaslew, flush_aaslew, create_aamix, destroy_aamix
// (B.2); xMixAudio, upslew, downslew, xaamix, flush_mix_ring
// (B.3).
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
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <avrt.h>     // AvSetMmThreadCharacteristics + AvSetMmThreadPriority
#  pragma comment(lib, "avrt.lib")
#endif

namespace lyra::wdsp {

// Forward declaration so create_aamix can call start_mixthread
// (defined further down in the file alongside the other Stage B.4
// threading bodies).  xaamix is already declared in AAMix.h
// (public API); flush_mix_ring is declared with its definition.
static void start_mixthread(AAMix* a);

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
    // Stage B.4 restored: if any input is marked active in the
    // initial mask, spin up mix_main immediately so the mixer is
    // self-driving as soon as data starts flowing.
    if (a->nactive > 0) {
        start_mixthread(a);
    }

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

    // Reference aamix.c:215-218 — thread-stop dance.
    //   1. Clear run bit so mix_main's while-check exits next iter.
    //   2. Release every per-input Ready semaphore once to unblock
    //      mix_main if it is parked inside the AND-wait
    //      (sequential acquire loop) waiting on a stream that has
    //      no producer running.
    //   3. Reference uses Sleep(2) as a "thread should be dead by
    //      now" grace period; Lyra-cpp uses request_stop() + the
    //      explicit join() below for deterministic teardown
    //      (the std::jthread dtor would join implicitly anyway
    //      via RAII when `delete a` runs, but joining BEFORE the
    //      resampler destruction avoids any race where the dying
    //      thread last-touches the resampler handles we're about
    //      to destroy).
    a->run.fetch_and(~1u, std::memory_order_release);
    for (int i = 0; i < a->ninputs; ++i) {
        if (a->Ready[i]) a->Ready[i]->release();
    }
    if (a->mix_thread.joinable()) {
        a->mix_thread.request_stop();
        a->mix_thread.join();
    }

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
static void flush_mix_ring(AAMix* a, int stream)
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

// =========================================================================
// Threading + slew orchestration — reference aamix.c:32-55 + :471-505.
// =========================================================================

// Reference aamix.c:32-49 — mix_main(pargs).
//
// The mixer thread entry.  Win32 MMCSS Pro Audio characteristic is
// requested (falls back to THREAD_PRIORITY_HIGHEST) so the mixer
// gets scheduled ahead of background work — same posture as the
// reference + the Lyra-cpp TxDspWorker convention.
//
// Loop:
//   while (run bit set AND no stop request):
//     AND-wait: acquire every active Ready semaphore in sequence
//       (the sequential-acquire equivalent of
//       WaitForMultipleObjects(nactive, Aready, TRUE, INFINITE)).
//     If stop requested after the wait, exit (close_mixer /
//       destroy_aamix release every Ready semaphore once to
//       unblock parked acquires before clearing run / stop).
//     Call xaamix(a) to mix-and-output into a->out.
//     Invoke the operator-supplied Outbound callback with the
//       fresh output frame.
//
// Reference uses _endthread() at loop exit; std::jthread handles
// thread teardown via the function's natural return.
static void mix_main(std::stop_token st, AAMix* a)
{
#ifdef _WIN32
    // Reference aamix.c:34-37.
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    if (hTask != nullptr) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
#endif

    // Reference aamix.c:41-47 — main loop.
    while (!st.stop_requested() &&
           (a->run.load(std::memory_order_acquire) & 1u) != 0u) {
        // AND-wait: acquire each active Ready semaphore.  The
        // reference uses one WaitForMultipleObjects call with
        // TRUE (= AND) and INFINITE; the sequential-acquire loop
        // is semantically equivalent — every element must be
        // ready before xaamix can produce one output frame, and
        // any ordering of permit grants yields the same outcome.
        const int n = a->nactive;
        for (int i = 0; i < n; ++i) {
            if (a->aready[i] != nullptr) {
                a->aready[i]->acquire();
            }
        }

        // Re-check stop + run after the (potentially long) wait
        // so close_mixer / destroy_aamix can wake the thread by
        // releasing semaphores AND clearing run / requesting stop.
        if (st.stop_requested() ||
            (a->run.load(std::memory_order_acquire) & 1u) == 0u) {
            break;
        }

        // Reference aamix.c:44 — pump body.
        xaamix(a);

        // Reference aamix.c:45 — operator-supplied output dispatch.
        if (a->Outbound) {
            a->Outbound(a->outbound_id, a->outsize, a->out.data());
        }
    }

#ifdef _WIN32
    if (hTask != nullptr) AvRevertMmThreadCharacteristics(hTask);
#endif
    // jthread joins on natural return; no _endthread() needed.
}

// Reference aamix.c:51-55 — start_mixthread(a).
//
// Spawns mix_main as a std::jthread member of the AAMix.  The
// reference uses _beginthread (which returns a HANDLE the
// reference discards); Lyra-cpp stores the jthread so destroy_aamix
// + close_mixer can request_stop() + join() deterministically.
static void start_mixthread(AAMix* a)
{
    // Ensure a stale dead thread is joined before reusing the
    // slot (defensive — open_mixer also runs through here on
    // every SetAAudioMixState toggle).
    if (a->mix_thread.joinable()) {
        a->mix_thread.request_stop();
        a->mix_thread.join();
    }
    a->mix_thread = std::jthread(mix_main, a);
}

// Reference aamix.c:471-491 — close_mixer(a).
//
// Slewed-shutdown half of the SetAAudioMixState{,s} +
// SetAAudioRing{In,Out}size + SetAAudioOutRate atomic
// "close-edit-open" sequence.  Steps (reference comments
// preserved verbatim in the inline annotations):
//
//   1. Set slew.dflag so the next xaamix invocation triggers
//      downslew over the output buffer.
//   2. Block on slew.dwait until downslew completes (or timeout
//      if no data is flowing — the dtimeout was computed in
//      create_aaslew as 1000ms*(tdelaydown+tslewdown)+2).
//   3. Clear slew.dflag so a stale dflag can't trigger downslew
//      again on the first frame after the eventual re-open.
//   4. Clear accept bits on all inputs — gates the xMixAudio
//      entry point so no fresh samples reach the rings during
//      the close window.
//   5. Sleep(1) — grace period for any xMixAudio caller that
//      has passed the accept-bit check and is on its way into
//      cs_in[stream]; let them reach the lock.
//   6. Take cs_in[i] on every input — blocks until any
//      in-flight xMixAudio call finishes its ring write.  These
//      locks are HELD across close_mixer's return; open_mixer
//      releases them.
//   7. Take cs_out — blocks the mixer thread at the top of
//      xaamix (xaamix's first line takes cs_out).  Held across
//      close_mixer's return; open_mixer releases.
//   8. Sleep(25) — wait for the mixer thread to actually arrive
//      at the top of its main loop where the cs_out gate is.
//   9. Clear run bit (the "trap" the reference comment names).
//  10. Release Ready[i] semaphores once each — wakes mix_main
//      from the AND-wait so it can proceed to the trap.
//  11. Release cs_out — lets the mixer thread pass into xaamix,
//      where the cleared run bit returns immediately.
//  12. Sleep(2) — wait for the mixer thread to exit.  Lyra-cpp
//      adds a deterministic join() instead of relying on Sleep,
//      while preserving the reference's Sleep(2) as a guard
//      bound just in case.
//  13. flush_mix_ring on every input to zero the rings + drain
//      Ready semaphore permits + flush the WDSP resampler.
//
// **Lock-without-release pattern**: steps 6-7 take std::mutex
// locks that step 5+6 of open_mixer release.  This is the only
// place in the entire AAMix port that uses raw lock()/unlock()
// instead of scoped_lock; the close-edit-open atom requires the
// locks to straddle a function boundary.  The reference uses
// CRITICAL_SECTION the same way (Enter in close, Leave in open).
//
// **[Lyra-native] join-vs-Sleep**: reference Sleep(2) is replaced
// by explicit request_stop() + join() so destruction order is
// deterministic.  Sleep(2) is kept as a safety guard in case a
// future implementation regression breaks the join() invariant
// — operationally a no-op when join() succeeds.
void close_mixer(AAMix* a)
{
    // 1-3: downslew + wait + clear flag.
    a->slew.dflag.fetch_or(1u, std::memory_order_release);
    if (a->slew.dwait) {
        (void)a->slew.dwait->try_acquire_for(
            std::chrono::milliseconds(a->slew.dtimeout));
    }
    a->slew.dflag.fetch_and(~1u, std::memory_order_release);

    // 4: shut accept gates on all inputs.
    for (int i = 0; i < a->ninputs; ++i) {
        a->accept[i].fetch_and(~1u, std::memory_order_release);
    }

    // 5: grace period for xMixAudio callers past the gate.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // 6: take cs_in[i] for every input — held across return.
    for (int i = 0; i < a->ninputs; ++i) {
        a->cs_in[i].lock();
    }

    // 7: take cs_out — blocks mixer at top of xaamix.  Held
    // across return.
    a->cs_out.lock();

    // 8: wait for mixer to arrive at the top of the main loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // 9: clear run bit (the trap).
    a->run.fetch_and(~1u, std::memory_order_release);

    // 10: wake mixer from AND-wait by releasing each Ready once.
    for (int i = 0; i < a->ninputs; ++i) {
        if (a->Ready[i]) a->Ready[i]->release();
    }

    // 11: let mixer pass into xaamix (where the trap returns).
    a->cs_out.unlock();

    // 12: wait for mixer to die.  Reference uses Sleep(2);
    // Lyra-cpp does explicit join() for determinism.
    if (a->mix_thread.joinable()) {
        a->mix_thread.request_stop();
        // Wake again in case the thread is parked in the AND-wait
        // having missed step 10 (race window: thread released
        // permits but blocked on a different semaphore that has
        // no producer — open_mixer must arm new producers).  The
        // jthread::join below will eventually return once
        // mix_main observes stop_requested after acquiring all
        // sema permits we've been releasing.
        for (int i = 0; i < a->ninputs; ++i) {
            if (a->Ready[i]) a->Ready[i]->release();
        }
        a->mix_thread.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // 13: restore rings to pristine state for the next open.
    for (int i = 0; i < a->ninputs; ++i) {
        flush_mix_ring(a, i);
    }
}

// Reference aamix.c:493-505 — open_mixer(a).
//
// Slewed-startup half of the close-edit-open atom.  Steps:
//
//   1. Set slew.uflag so the first output frame after restart
//      triggers upslew.
//   2. Set run bit (clear the trap close_mixer set).
//   3. If any input is active, start a fresh mix_main thread.
//   4. Release cs_in[i] in reverse order — re-enables xMixAudio
//      entries.
//   5. Re-arm accept bits on every input that is in the active
//      mask, also in reverse order.
//   6. Block on slew.uwait until upslew completes (or utimeout
//      if no data is flowing).
//
// Reverse-order release of cs_in[i] matches reference aamix.c:
// 499-503 byte-for-byte; the order does not affect correctness
// (the locks were taken in forward order in close_mixer; they
// can be released in any order) but the reference does it
// reversed and the port preserves the loop direction so a
// future operator inspecting either source sees the same
// pattern.
void open_mixer(AAMix* a)
{
    // 1: arm upslew.
    a->slew.uflag.fetch_or(1u, std::memory_order_release);

    // 2: clear the run-bit trap close_mixer set.
    a->run.fetch_or(1u, std::memory_order_release);

    // 3: start mix_main if anything is active.
    if (a->nactive > 0) {
        start_mixthread(a);
    }

    // 4: release cs_in[i] in reverse order (matches reference).
    for (int i = a->ninputs - 1; i >= 0; --i) {
        a->cs_in[i].unlock();
    }

    // 5: re-arm accept bits for inputs in the active mask.
    const std::uint32_t active_mask =
        a->active.load(std::memory_order_acquire);
    for (int i = a->ninputs - 1; i >= 0; --i) {
        if ((active_mask & (1u << i)) != 0u) {
            a->accept[i].fetch_or(1u, std::memory_order_release);
        }
    }

    // 6: block on uwait until upslew completes (or timeout).
    if (a->slew.uwait) {
        (void)a->slew.uwait->try_acquire_for(
            std::chrono::milliseconds(a->slew.utimeout));
    }
}

// =========================================================================
// MIXER PROPERTIES — reference aamix.c:507-699.
// =========================================================================

// Helper — resolves the AAMix from the (ptr, id) pair per the
// reference's `if (ptr == 0) a = paamix[id]; else a = (AAMIX)ptr`
// pattern.  Returns nullptr if both ptr is null and id is out
// of range, so callers can safely guard with `if (!a) return`.
static AAMix* resolve_aamix(AAMix* ptr, int id)
{
    if (ptr != nullptr) return ptr;
    if (id < 0 || id >= MAX_EXT_AAMIX) return nullptr;
    return paamix[id];
}

// Reference aamix.c:513-519 — SetAAudioMixOutputPointer.
void SetAAudioMixOutputPointer(
    AAMix* ptr, int id,
    std::function<void(int id, int nsamples, double* buff)> Outbound)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    a->Outbound = std::move(Outbound);
}

// Reference aamix.c:521-546 — SetAAudioMixState(ptr, id, stream, state).
//
// Edits the active mask + nactive count + aready[] list + accept
// bits for ONE stream.  Wrapped in a close_mixer/open_mixer atom
// so any in-flight mix is slewed-out + ringbufs flushed before
// the edit, and slewed back in after.  Idempotent — early-return
// if the requested state already matches the current bit.
void SetAAudioMixState(AAMix* ptr, int id, int stream, int state)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    const std::uint32_t cur_active =
        a->active.load(std::memory_order_acquire);
    const int cur_state =
        static_cast<int>((cur_active >> stream) & 1u);
    if (cur_state == state) return;

    close_mixer(a);
    if (state) {
        a->active.fetch_or(1u << stream, std::memory_order_release);
    } else {
        a->active.fetch_and(~(1u << stream), std::memory_order_release);
    }
    // Rebuild nactive + aready[] + accept flags from the new mask.
    a->nactive = 0;
    const std::uint32_t new_active =
        a->active.load(std::memory_order_acquire);
    for (int i = 0; i < a->ninputs; ++i) {
        if ((new_active & (1u << i)) != 0u) {
            a->aready[a->nactive++] = a->Ready[i].get();
            a->accept[i].fetch_or(1u, std::memory_order_release);
        } else {
            a->accept[i].fetch_and(~1u, std::memory_order_release);
        }
    }
    open_mixer(a);
}

// Reference aamix.c:548-582 — SetAAudioMixStates(ptr, id, streams,
// states).  Batched-set variant: `streams` mask says which bits
// to change, `states` mask says what to set them to.  Idempotent
// guard: early-return if the masked subset of `active` already
// matches `states & streams`.
void SetAAudioMixStates(AAMix* ptr, int id, int streams, int states)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    const std::uint32_t cur_active =
        a->active.load(std::memory_order_acquire);
    const std::uint32_t want = static_cast<std::uint32_t>(states) &
                               static_cast<std::uint32_t>(streams);
    const std::uint32_t cur  = cur_active &
                               static_cast<std::uint32_t>(streams);
    if (cur == want) return;

    close_mixer(a);
    for (int i = 0; i < a->ninputs; ++i) {
        if (((streams >> i) & 1) != 0) {
            if (((states >> i) & 1) != 0) {
                a->active.fetch_or(1u << i, std::memory_order_release);
            } else {
                a->active.fetch_and(~(1u << i),
                                    std::memory_order_release);
            }
        }
    }
    a->nactive = 0;
    const std::uint32_t new_active =
        a->active.load(std::memory_order_acquire);
    for (int i = 0; i < a->ninputs; ++i) {
        if ((new_active & (1u << i)) != 0u) {
            a->aready[a->nactive++] = a->Ready[i].get();
            a->accept[i].fetch_or(1u, std::memory_order_release);
        } else {
            a->accept[i].fetch_and(~1u, std::memory_order_release);
        }
    }
    open_mixer(a);
}

// Reference aamix.c:584-594 — SetAAudioMixWhat.
//
// Sets/clears bit `stream` of the `what` mask.  Does NOT need a
// close/open atom — `what` is read atomically by xaamix per
// frame.  Operator-facing MUTE/UNMUTE control for a specific
// input.
void SetAAudioMixWhat(AAMix* ptr, int id, int stream, int state)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    if (state) {
        a->what.fetch_or(1u << stream, std::memory_order_release);
    } else {
        a->what.fetch_and(~(1u << stream), std::memory_order_release);
    }
}

// Reference aamix.c:596-608 — SetAAudioMixVolume.
//
// Master volume.  Takes cs_out so the recompute of tvol[] is
// atomic w.r.t. xaamix reading tvol[] mid-mix.
void SetAAudioMixVolume(AAMix* ptr, int id, double volume)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    std::scoped_lock lock(a->cs_out);
    a->volume = volume;
    for (int i = 0; i < AAMIX_MAX_INPUTS; ++i) {
        a->tvol[i] = a->volume * a->vol[i];
    }
}

// Reference aamix.c:610-620 — SetAAudioMixVol.
//
// Per-input volume.  Same cs_out atomicity requirement as
// SetAAudioMixVolume.
void SetAAudioMixVol(AAMix* ptr, int id, int stream, double vol)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    std::scoped_lock lock(a->cs_out);
    a->vol [stream] = vol;
    a->tvol[stream] = a->vol[stream] * a->volume;
}

// Reference aamix.c:622-642 — SetAAudioRingInsize.
//
// Change `ringinsize` while data may be flowing.  Wrapped in
// close/open.  Per-input resampler->size is updated in place
// (the reference does direct `->size` writes; the public WDSP
// resample struct is replicated in Resample.h so the cast is
// well-defined); resampler-output buffer is reallocated.
void SetAAudioRingInsize(AAMix* ptr, int id, int size)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    close_mixer(a);
    a->ringinsize = size;
    for (int i = 0; i < a->ninputs; ++i) {
        int rs_size;
        if (a->inrate[i] > a->outrate) {
            rs_size = a->ringinsize * (a->inrate[i] / a->outrate);
        } else {
            rs_size = a->ringinsize / (a->outrate / a->inrate[i]);
        }
        resample* rsmp = static_cast<resample*>(a->rsmp[i]);
        if (rsmp != nullptr) {
            rsmp->size = rs_size;
        }
        a->resampbuff[i].assign(static_cast<size_t>(2 * a->ringinsize),
                                0.0);
        if (rsmp != nullptr) {
            rsmp->out = a->resampbuff[i].data();
        }
    }
    open_mixer(a);
}

// Reference aamix.c:644-654 — SetAAudioRingOutsize.
//
// Change `outsize` while data may be flowing.  Wrapped in
// close/open.  Output buffer reallocated.
void SetAAudioRingOutsize(AAMix* ptr, int id, int size)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    close_mixer(a);
    a->outsize = size;
    a->out.assign(static_cast<size_t>(2 * a->outsize), 0.0);
    open_mixer(a);
}

// Reference aamix.c:656-681 — SetAAudioOutRate.
//
// Change `outrate` while data may be flowing.  Wrapped in
// close/open.  Per-input resamplers are destroyed + recreated
// with the new rate (and re-evaluated run flag based on
// in_rate==out_rate match).
void SetAAudioOutRate(AAMix* ptr, int id, int rate)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    close_mixer(a);
    a->outrate = rate;
    if (a->wdsp != nullptr) {
        const lyra::dsp::WdspApi& api = a->wdsp->api();
        for (int i = 0; i < a->ninputs; ++i) {
            int run;
            int size;
            if (a->rsmp[i] != nullptr && api.destroy_resample != nullptr) {
                api.destroy_resample(a->rsmp[i]);
                a->rsmp[i] = nullptr;
            }
            if (a->inrate[i] != a->outrate) run = 1;
            else                            run = 0;
            if (a->inrate[i] > a->outrate) {
                size = a->ringinsize * (a->inrate[i] / a->outrate);
            } else {
                size = a->ringinsize / (a->outrate / a->inrate[i]);
            }
            a->resampbuff[i].assign(
                static_cast<size_t>(2 * a->ringinsize), 0.0);
            if (api.create_resample != nullptr) {
                a->rsmp[i] = api.create_resample(
                    run, size, nullptr, a->resampbuff[i].data(),
                    a->inrate[i], a->outrate, 0.0, 0, 1.0);
                // Reference aamix.c:678 redundantly assigns
                // `->out` after create_resample (which already
                // takes `out` as the 4th arg).  Preserved
                // verbatim for byte-for-byte parity with the
                // reference behaviour.
                resample* rsmp = static_cast<resample*>(a->rsmp[i]);
                if (rsmp != nullptr) {
                    rsmp->out = a->resampbuff[i].data();
                }
            }
        }
    }
    open_mixer(a);
}

// Reference aamix.c:683-699 — SetAAudioStreamRate.
//
// Change ONE stream's input rate.  The reference NOTE comment
// at :684 is load-bearing: "you must set the stream state to
// INACTIVE before using this function!"  This function does
// NOT wrap close/open — the caller is expected to have
// SetAAudioMixState(..., 0)'d the stream first, then call this,
// then SetAAudioMixState(..., 1).
void SetAAudioStreamRate(AAMix* ptr, int id, int mixinid, int rate)
{
    AAMix* a = resolve_aamix(ptr, id);
    if (a == nullptr) return;
    if (a->wdsp == nullptr) return;
    const lyra::dsp::WdspApi& api = a->wdsp->api();
    a->inrate[mixinid] = rate;
    if (a->rsmp[mixinid] != nullptr && api.destroy_resample != nullptr) {
        api.destroy_resample(a->rsmp[mixinid]);
        a->rsmp[mixinid] = nullptr;
    }
    int run;
    int size;
    if (a->inrate[mixinid] != a->outrate) run = 1;
    else                                  run = 0;
    if (a->inrate[mixinid] > a->outrate) {
        size = a->ringinsize * (a->inrate[mixinid] / a->outrate);
    } else {
        size = a->ringinsize / (a->outrate / a->inrate[mixinid]);
    }
    if (api.create_resample != nullptr) {
        a->rsmp[mixinid] = api.create_resample(
            run, size, nullptr, a->resampbuff[mixinid].data(),
            a->inrate[mixinid], a->outrate, 0.0, 0, 1.0);
    }
}

} // namespace lyra::wdsp
