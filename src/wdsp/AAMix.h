// Lyra-cpp — AAMix.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/aamix.h: struct aamix typedef + slew sub-struct
//                            + 14 public function decls
//   - ChannelMaster/aamix.c: implementation (ported in Stage B.2+)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// **C → C++23 idiom translations applied** (every translation
// preserves observable behaviour; see per-symbol comments for
// exceptions):
//
//   - `struct aamix` typedef       → `struct AAMix` with
//                                    in-class initializers
//                                    + RAII members.  Layout is
//                                    Lyra-private (free of any
//                                    DLL-ABI constraint); fields
//                                    re-ordered ONLY where the
//                                    C++ type forces it (e.g.
//                                    std::array/std::vector
//                                    placement); semantic
//                                    contract preserved.
//   - C unscoped slew states       → `enum class SlewState : int`
//                                    with explicit integer values
//                                    matching reference _slew
//                                    (aamix.c:57-67) byte-for-byte.
//   - `CRITICAL_SECTION cs_*`      → `std::mutex` (per cmaster
//                                    Stage A Q5 Option A
//                                    sign-off).  The reference
//                                    spin-count tuning of 2500
//                                    (InitializeCriticalSectionAnd
//                                    SpinCount) is discarded —
//                                    std::mutex implementations on
//                                    Windows already spin briefly
//                                    before kernel wait, so the
//                                    contention profile is
//                                    equivalent.
//   - `HANDLE Ready[32]` semaphore → `std::array<std::unique_ptr<
//                                    std::counting_semaphore<1000>>
//                                    , 32>` Ready[i].  max=1000
//                                    matches reference's
//                                    CreateSemaphore(0,0,1000,0)
//                                    at aamix.c:175.  unique_ptr
//                                    because std::counting_
//                                    semaphore has no default
//                                    constructor.
//   - `HANDLE Aready[32]` array of → `std::array<std::counting_
//     pointers-into-Ready                 semaphore<1000>*, 32>`
//                                    aready.  Holds raw pointers
//                                    into Ready[] for the active
//                                    inputs; the AND-wait below
//                                    iterates this list.
//   - `HANDLE uwait, dwait`        → `std::binary_semaphore`
//     (CreateSemaphore max=1)        (== std::counting_semaphore
//                                    <1>) — matches max=1.
//   - `WaitForMultipleObjects(N,    → sequential `aready[i]->
//      Aready, TRUE, INFINITE)`       acquire()` loop over [0,
//                                    nactive).  Functionally
//                                    identical: AND-wait is N
//                                    sequential waits, any order
//                                    works (all N must be ready
//                                    before the mixer can produce
//                                    an output frame).  See
//                                    aamix.c:43 mix_main.
//   - `volatile long` + _Interlocked-> `std::atomic<std::uint32_t>`
//      BitTestAnd{Set,Reset} /        with fetch_or(1u<<bit) /
//      _InterlockedAnd / _Or          fetch_and(~(1u<<bit)) /
//                                    load() & mask / fetch_or /
//                                    fetch_and.  Used for: run,
//                                    accept[32], active, what,
//                                    slew.uflag, slew.dflag.
//                                    Atomics needed because these
//                                    cross thread boundaries
//                                    (xMixAudio producer threads,
//                                    mix_main consumer, Set*
//                                    setter threads).
//   - `malloc0(...)` + `_aligned_   → `std::vector<double>(size,
//      free(...)` for double buffers   0.0)` — zero-init matches
//                                    malloc0 semantics; RAII frees
//                                    on AAMix destruction.
//   - `_beginthread(mix_main, ...)` → `std::jthread` member.
//                                    stop_token augments (but does
//                                    not replace) the reference's
//                                    `run` bit for clean shutdown
//                                    wake-up on join.
//   - `Sleep(ms)`                  → `std::this_thread::sleep_for(
//                                    std::chrono::milliseconds(ms))`
//   - `AvSetMmThreadCharacteristics → Win32 platform-shim retained
//      ("Pro Audio")` MMCSS           inside `#ifdef _WIN32` —
//                                    matches the established Lyra
//                                    pattern (TxDspWorker etc.).
//   - C function-pointer `Outbound` → `std::function<void(int, int,
//                                    double*)>` (per docs/RULES.md
//                                    §5.8 signed-off translation;
//                                    mirrors CMaster Stage A's
//                                    OutboundCallback alias).
//   - `void* ptr` opaque AAMix      → `AAMix*` typed pointer.  The
//      handles in the public API      reference's `ptr == 0 ⇒
//                                    look-up via paamix[id]`
//                                    semantic is preserved
//                                    verbatim.
//   - `paamix[MAX_EXT_AAMIX]`       → `inline std::array<AAMix*,
//      central pointer bank           MAX_EXT_AAMIX> paamix` (in
//                                    AAMix.cpp).  Process-lifetime;
//                                    `create_aamix(id ≥ 0, ...)`
//                                    publishes into the bank;
//                                    `destroy_aamix(ptr=0, id)`
//                                    looks up.
//   - `__declspec(dllexport)` on    → DROPPED.  Lyra-cpp's AAMix
//      reference public functions     is in-process C++ code, not
//                                    a DLL-exported surface (the
//                                    cmaster pump invokes it
//                                    directly).  PORT macro elided.
//
// `[Lyra-native]` markers identify additions that are not part
// of the reference port (Stage B is pure port; no [Lyra-native]
// additions expected in B.1-B.5).
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.  See docs/RULES.md AA. 2026-06-08 Amendment for
// the project-level rule change that permits this port.
//
// ---------------------------------------------------------------
//
// **Stage B sub-commit plan** (operator-directed 2026-06-08
// after the Thetis port rule change):
//
//   Stage B.0 (SHIPPED `6938022`): resample (double, complex)
//     cdef + resolve into wdsp_native.  PE export-table walk +
//     compile + link all green.  Wire-inert (bindings populated
//     but unused).
//
//   Stage B.1 (THIS COMMIT): AAMix.h struct typedef + slew
//     sub-struct + enum SlewState + 14 function decls + full
//     attribution + idiom-translation log.  Companion
//     Resample.h replicates the WDSP public ABI struct so AAMix
//     can do direct `->in`/`->out`/`->size` field writes (the
//     upstream-faithful pattern aamix.c uses).  No
//     implementation yet (the .cpp body lands in B.2-B.5).
//     Wire-inert (decls only — nothing constructs an AAMix).
//
//   Stage B.2 (PENDING): create_aamix + destroy_aamix +
//     create_aaslew + destroy_aaslew + flush_aaslew.  Mixer
//     constructable but no pump thread yet.
//
//   Stage B.3 (PENDING): xMixAudio + flush_mix_ring +
//     xaamix pump + upslew + downslew state machines.
//
//   Stage B.4 (PENDING): close_mixer + open_mixer + mix_main
//     + start_mixthread (threading + slew orchestration).
//
//   Stage B.5 (PENDING): 10 Set* property setters + wire the
//     `SetAAudioMixOutputPointer(0,0,cb)` call into Stage A's
//     `CMaster::SendpOutboundRx` stub (the architectural
//     hand-off that Stage A wired the storage slot for).
//
//   Stage B.6 (DEFER): migrate the existing Lyra-native
//     audio_mixer to use the ported AAMix in the RX audio
//     routing path.  This is the wire-effective commit;
//     operator HL2 bench-gate matters here.
//
// Each B.x sub-stage ships independently with a build-green
// verify and a push.  HL2 bench gate per stage = wire-inert
// (RX audio path untouched) until B.6 (migration).

#pragma once

#include "wdsp/Resample.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

namespace lyra::dsp { class WdspNative; }

namespace lyra::wdsp {

// Reference aamix.c:29 — `#define MAX_EXT_AAMIX (4)`.  Process-wide
// central bank size.  Lyra-cpp keeps the same upper bound (one
// global AAMix instance is enough for the current RX path; the
// extra slots are reserved for future per-RX2 / per-monitor /
// per-VAC mixers per the same architecture the reference uses).
inline constexpr int MAX_EXT_AAMIX = 4;

// Reference aamix.h:40 — fixed compile-time upper bound on the
// number of inputs per mixer.  Reference uses bare `[32]` arrays
// throughout; Lyra-cpp lifts the literal into a named constant
// so the array sizing reads consistently.
inline constexpr int AAMIX_MAX_INPUTS = 32;

// Reference _slew enum at aamix.c:57-67.  Integer values match
// byte-for-byte so the .cpp state-machine switches port verbatim.
enum class SlewState : int {
    BEGIN     = 0,
    DELAYUP   = 1,
    UPSLEW    = 2,
    ON        = 3,
    DELAYDOWN = 4,
    DOWNSLEW  = 5,
    ZERO      = 6,
    OFF       = 7,
};

// Reference inner `slew` anonymous struct at aamix.h:62-84.
// Promoted to a named struct in Lyra-cpp purely for clearer
// member access; field set + types + defaults match reference
// (with the documented idiom translations applied).
struct AaSlew {
    // Operator-supplied slew timing (seconds).
    double tdelayup   = 0.0;   // delay before upslew
    double tslewup    = 0.0;   // upslew time
    double tdelaydown = 0.0;   // delay before downslew
    double tslewdown  = 0.0;   // downslew time

    // State-machine counters / state.
    SlewState ustate = SlewState::BEGIN;  // state of upslew function
    SlewState dstate = SlewState::BEGIN;  // state of downslew function
    int ucount = 0;                       // upslew counter
    int dcount = 0;                       // downslew counter
    int ndelup   = 0;                     // number of samples for delayup
    int ntup     = 0;                     // number of samples for upslew
    int ndeldown = 0;                     // number of samples for delaydown
    int ntdown   = 0;                     // number of samples for downslew

    // Cosine envelope coefficient tables (computed in create_aaslew).
    std::vector<double> cup;              // upslew coefficients
    std::vector<double> cdown;            // downslew coefficients

    // Flag bits — set by close_mixer / open_mixer (one-bit each).
    // Cross-thread: producer setters + mix_main consumer.
    std::atomic<std::uint32_t> uflag{0};  // set when upslew to proceed
    std::atomic<std::uint32_t> dflag{0};  // set when downslew to proceed

    // Wait gates the close_mixer / open_mixer callers block on
    // until the corresponding slew completes.  CreateSemaphore
    // max=1 → std::binary_semaphore (== counting_semaphore<1>).
    // Initial count 0 — matches CreateSemaphore(0,0,1,0).
    // unique_ptr because binary_semaphore has no default ctor.
    std::unique_ptr<std::binary_semaphore> uwait;
    std::unique_ptr<std::binary_semaphore> dwait;

    // Operator timeouts (ms) used when no data is flowing —
    // computed in create_aaslew from t{delay,slew}{up,down} +
    // 2 ms guard, matches reference aamix.c:105-106.
    int dtimeout = 0;
    int utimeout = 0;
};

// Reference `struct _aamix` typedef at aamix.h:32-85.
//
// Lyra-cpp uses `struct AAMix` (not class) to mirror the
// reference's plain-aggregate posture — every field is public
// and the operations are free functions (matching the reference
// API surface).  In-class initializers replace the reference's
// implicit zero-init via malloc0.
struct AAMix {
    // Reference aamix.h:34-35.
    int id           = 0;   // id of this aamixer
    int outbound_id  = 0;   // id to use in the Outbound() call

    // Reference aamix.h:36 — thread runs when set to 1.
    // Atomic for cross-thread mix_main / destroy_aamix coordination.
    std::atomic<std::uint32_t> run{0};

    // Reference aamix.h:37 — ring accepts data when set to 1.
    // One atomic per input slot; bit 0 of each is the accept flag.
    std::array<std::atomic<std::uint32_t>, AAMIX_MAX_INPUTS> accept{};

    // Reference aamix.h:38-41.
    int ringinsize = 0;     // input size to rings, complex samples
    int outsize    = 0;     // output size, complex samples
    int ninputs    = 0;     // number of inputs (consecutive from 0)
    int rsize      = 0;     // total size of a ring, complex samples

    // Reference aamix.h:42 — `double* ring[32]`.  RAII via
    // std::vector; sized rsize * sizeof(complex) (==2 doubles)
    // = 2 * rsize doubles per stream, allocated in create_aamix.
    std::array<std::vector<double>, AAMIX_MAX_INPUTS> ring{};

    // Reference aamix.h:43 — `double* out`.  RAII via std::vector;
    // sized outsize * sizeof(complex) = 2 * outsize doubles.
    std::vector<double> out;

    // Reference aamix.h:44-46.  Bit-mask atomics; bit i = input i
    // active / wanted-in-mix.  Use fetch_or / fetch_and for set/
    // clear, load() & (1u<<i) for test.
    std::atomic<std::uint32_t> active{0};   // one bit per active input
    int nactive = 0;                        // count of active inputs
    std::atomic<std::uint32_t> what{0};     // one bit per item to mix

    // Reference aamix.h:47-49.  Per-input + master gain scaling.
    std::array<double, AAMIX_MAX_INPUTS> vol{};   // per-input volume
    std::array<double, AAMIX_MAX_INPUTS> tvol{};  // final = vol * master
    double volume = 0.0;                          // master volume

    // Reference aamix.h:50-52 — per-ring write/read indices +
    // unqueued-sample counter (used to release the Ready semaphore
    // when ≥outsize samples have accumulated).
    std::array<int, AAMIX_MAX_INPUTS> inidx{};
    std::array<int, AAMIX_MAX_INPUTS> outidx{};
    std::array<int, AAMIX_MAX_INPUTS> unqueuedsamps{};

    // Reference aamix.h:53-54.  Per-input semaphore signalled when
    // ≥outsize samples are queued in that ring.  mix_main waits on
    // the active subset (aready) before producing each output frame.
    // unique_ptr because std::counting_semaphore has no default ctor;
    // max permits = 1000 matches CreateSemaphore(0, 0, 1000, 0).
    std::array<std::unique_ptr<std::counting_semaphore<1000>>,
               AAMIX_MAX_INPUTS> Ready{};

    // Raw pointers into Ready[] for the active inputs; rebuilt in
    // create_aamix + every SetAAudioMixState{,s} state-change.
    // mix_main iterates [0, nactive) and acquires each in turn —
    // the sequential AND-wait equivalent of
    // `WaitForMultipleObjects(nactive, Aready, TRUE, INFINITE)`.
    std::array<std::counting_semaphore<1000>*, AAMIX_MAX_INPUTS> aready{};

    // Reference aamix.h:55-56.  CRITICAL_SECTION → std::mutex.
    // cs_in[i] protects xMixAudio's per-stream write path; cs_out
    // protects xaamix's mix-and-output path + open/close gates.
    std::array<std::mutex, AAMIX_MAX_INPUTS> cs_in{};
    std::mutex cs_out;

    // Reference aamix.h:57-60.  Per-input WDSP resampler handle
    // (opaque pointer returned by api().create_resample), its
    // configured input rate, the shared output rate, and the
    // intermediate resampler-output buffer the resampler writes
    // into before xMixAudio's ring-copy step.
    std::array<void*, AAMIX_MAX_INPUTS> rsmp{};
    std::array<int,   AAMIX_MAX_INPUTS> inrate{};
    int outrate = 0;
    std::array<std::vector<double>, AAMIX_MAX_INPUTS> resampbuff{};

    // Reference aamix.h:61.  Operator-supplied output callback —
    // mix_main invokes it after every xaamix pump with the freshly
    // mixed `out` buffer.  std::function per the docs/RULES.md
    // §5.8 idiom.  Signature matches CMaster Stage A
    // OutboundCallback exactly so SetAAudioMixOutputPointer can
    // accept the same callable type the SendpOutboundRx setter
    // stores.
    std::function<void(int id, int nsamples, double* buff)> Outbound;

    // Reference aamix.h:62-84.  Slew sub-struct (factored into a
    // named type for clearer member access).
    AaSlew slew;

    // [Lyra-native] mix_main thread handle.  The reference uses
    // _beginthread + _endthread() with the `run` bit as the kill
    // flag; Lyra-cpp uses std::jthread with stop_token augmenting
    // the same `run` bit.  Lives in the AAMix so destruction joins
    // automatically (RAII).
    std::jthread mix_thread;

    // [Lyra-native] DLL handle for the resampler API
    // (api().create_resample / destroy_resample / flush_resample /
    // xresample resolved at Lyra startup).  Required because
    // Lyra-cpp consumes wdsp.dll via dynamic LoadLibrary +
    // GetProcAddress (vs the reference's static link), so the
    // function pointers are not file-scope globals — they are
    // members of the WdspNative singleton.  Storing the singleton
    // pointer on the AAMix lets every operation (create_aamix,
    // destroy_aamix, xMixAudio, SetAAudioRingInsize,
    // SetAAudioOutRate, SetAAudioStreamRate) reach the resampler
    // calls without a global lookup.  Single-additional-field
    // divergence from the reference; mixer semantics unchanged.
    // Set by create_aamix; never reassigned; never dereferenced
    // for ownership (the singleton outlives every AAMix per the
    // Lyra-cpp lifecycle).
    lyra::dsp::WdspNative* wdsp = nullptr;
};

// ===== Public API (matches aamix.h:87-122 byte-for-byte) =====
//
// All 14 functions ported verbatim from the reference public
// surface.  The `void* ptr` parameter pattern (ptr=0 → look up
// AAMix via paamix[id]) is preserved verbatim so the cmaster
// pump's call sites (Stage D) port unchanged.  In Lyra-cpp the
// pointer type is the typed `AAMix*` rather than `void*` —
// callers can always pass `nullptr` to trigger the id-based
// look-up.

// Reference aamix.h:87-104 — `create_aamix(...)` constructor.
// Allocates an AAMix, sizes all per-input vectors + resamplers,
// initializes semaphores + mutexes + slew, registers in paamix[id]
// if id >= 0, starts mix_main if nactive > 0.  Returns the new
// AAMix (== the value stored in paamix[id] when id >= 0).
//
// **[Lyra-native]** extra leading param `wdsp` — the resolved
// WdspNative singleton, so create_aamix + subsequent operations
// can reach api().create_resample(...) without a global lookup.
// Documented in the AAMix struct's `wdsp` field comment.  Single-
// param divergence from reference; mixer semantics unchanged.
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
    double tslewdown);

// Reference aamix.h:106.  Stops mix_main, destroys all per-input
// state, releases the slew object, frees the AAMix.
void destroy_aamix(AAMix* ptr, int id);

// Reference aamix.h:108.  The mix-and-output pump body called by
// mix_main on every wake-up.  Public for unit-test access.
void xaamix(AAMix* a);

// Reference aamix.h:110.  Per-input sample-push entry point —
// resamples (if rates differ) into the ring + releases the Ready
// semaphore when ≥outsize samples have accumulated.  Called by
// upstream pump (Stage D) for every block of input samples.
void xMixAudio(AAMix* ptr, int id, int stream, double* data);

// Reference aamix.h:112.  Operator-supplied output dispatcher;
// stored on the AAMix.  Called via SendpOutboundRx (Stage A
// CMaster.cpp stub) once Stage B.5 wires it.
void SetAAudioMixOutputPointer(
    AAMix* ptr, int id,
    std::function<void(int id, int nsamples, double* buff)> Outbound);

// Reference aamix.h:114-122.  Property setters — operator-visible
// runtime controls on the mixer.  The reference `__declspec
// (dllexport)` annotations are dropped because Lyra-cpp's AAMix
// is consumed in-process (the cmaster pump calls these directly).
void SetAAudioMixVol      (AAMix* ptr, int id, int stream, double vol);
void SetAAudioMixVolume   (AAMix* ptr, int id, double volume);
void SetAAudioMixState    (AAMix* ptr, int id, int stream, int state);
void SetAAudioMixStates   (AAMix* ptr, int id, int streams, int states);
void SetAAudioMixWhat     (AAMix* ptr, int id, int stream, int state);
void SetAAudioRingInsize  (AAMix* ptr, int id, int size);
void SetAAudioRingOutsize (AAMix* ptr, int id, int size);
void SetAAudioOutRate     (AAMix* ptr, int id, int rate);
void SetAAudioStreamRate  (AAMix* ptr, int id, int mixinid, int rate);

} // namespace lyra::wdsp

// =========================================================================
// Contract notes — reference aamix.h:126-201 verbatim (operationally
// load-bearing, preserved here so callers reading AAMix.h see the same
// pre-conditions the reference operators / cmaster pump rely on).
// =========================================================================
//
// Accessing the Mixer Functions.
// A data structure is stored for each instance of the mixer.  A pointer to
// the data structure is required to use the corresponding instance of the
// mixer.  That pointer can either be stored in a central bank of pointers
// and accessed using an 'id', OR, the owner of the mixer can store the
// pointer and use it in the various calls to the mixer.  In using the
// create_aamix(...) function, if a negative value is supplied for the 'id',
// a pointer will NOT be stored in the central bank; the function will also
// ALWAYS return the pointer which may be used or ignored.  In calls to other
// mixer functions, there are parameters for both 'id' and 'ptr'.  If
// ptr == nullptr, the id will be used.  If ptr != nullptr, the pointer,
// supplied as 'ptr', will be used.
//
// The audio mixer is created with a specific number of inputs, 'ninputs'.
// 'ninputs' should be the MAXIMUM number of data streams that the creator
// will need to mix.
//
// Of these total 'ninputs', some can be active and some inactive at any
// point in time.  SetAAudioMixState() is used to set whether an input is
// active or not.  Data MUST CONTINUOUSLY flow into all the active inputs.
// To be able to mix the input samples and produce output, the mixer MUST
// have samples from each input stream.  Therefore, if there is no data
// flowing in from even one active input, the mixer will be unable to
// produce any output.
//
// SetAAudioMixState() is intended to be used, for example, when a new
// receiver is created in the radio or when a receiver is removed from the
// radio.  It is NOT intended for use just to turn OFF/ON whether a stream
// is currently being mixed into the output stream.  Why?  Because its
// transition is much slower than using SetAAudioMixWhat().
//
// Note also that if a stream is marked active and data is not yet flowing,
// the mixer will block until data flows.
//
// Having continuous data flow to all inputs does NOT imply that all inputs
// will always be mixed into the output.  The content of the output, at any
// given time, is determined by which bits are set in the word 'what'.  Note
// that 'what' can be changed at any time, while data is flowing.  Turning
// a bit OFF/ON in 'what' could be used, for example, to MUTE/UNMUTE a
// particular software receiver.  Call SetAAudioMixWhat() to use this
// functionality.
//
// Individual volume settings are provided for each input.  There is also a
// master volume setting that applies to all inputs.  The individual and
// master volume settings can be changed anytime, while data is flowing.
//
// There is a ring buffer corresponding to each input stream.  When the
// ring for each input has at least the number of samples required for one
// 'outsize' output buffer, mixing proceeds and the output buffer is filled.
// A resampler is provided for each input and is activated if the input
// sample rate for that input does not match the specified output sample
// rate for the mixer.  The resampling occurs BEFORE data is entered into
// the ring.  This implies that the input size 'ringinsize' is the same
// for each ring.
//
// The mixer supports changing 'outsize' and 'ringinsize' while data is
// flowing.  However, it is anticipated that these parameters would likely
// be set when the radio is created and not be changed after that.
//
// Mixer slewing.
// When the mixer is to be 'closed' to add or remove an input using
// SetAAudioMixState(), provision is made to downslew before stopping the
// output and to upslew when the mixer is again 'opened'.
//
// When data is continuously flowing for all active inputs, operation is as
// follows.  SetAAudioMixState() calls close_mixer() where the
// a->slew.dflag is set.  Because this flag is set, for subsequent
// processing of data buffers in xaamix(), downslew() is called.  When the
// downslew completes, the downslew() function resets the a->slew.dflag.
// In parallel with the downslewing activity in xaamix() and downslew(),
// the thread calling SetAAudioMixState() and subsequently close_mixer() is
// blocked waiting for semaphore a->slew.dwait.  This prevents further
// closure of the mixer until the downslew is complete.  Once the
// semaphore is released by the downslew() function, the mixer will be
// closed.  At that point, the changes in "MixState" are made and
// open_mixer() is then called.  a->slew.uflag is immediately set so that,
// when output buffers begin to flow in xaamix(), an upslew() will occur.
//
// At initial startup, when there is no data flowing, calls to
// SetAAudioMixState() can still be made without creating a permanent block
// on the calling thread.  Each of the semaphore waits (one for downslew
// and the other for upslew) has a timeout that is set to be slightly
// greater than the downslew or upslew time.
//
// NOTE: The above describes two distinct valid modes of operation:
// (1) no data is flowing during the entire close_mixer() and open_mixer()
// process, and (2) data is continually flowing during the entire
// close_mixer() and open_mixer() process.  Correct operation will NOT be
// preserved if data flow starts or stops during these operations.
//
// Calling SetAAudioMixState() on the same thread used to turn ON/OFF data
// flow (for example by setting the 'run' bit or setting 'enable' bits) can
// be used to keep this properly sorted out.  If data is NOT flowing, just
// do NOT start it until the call to SetAAudioMixState() returns.  If data
// IS flowing, simply do NOT turn it off until the call to
// SetAAudioMixState() returns.
