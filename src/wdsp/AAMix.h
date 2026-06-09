// Lyra-cpp — AAMix.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/aamix.h: struct aamix typedef + slew sub-struct
//                            + 14 public function decls
//   - ChannelMaster/aamix.c: implementation (~600 LOC)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT 2026-06-09
// (operator directive: no Lyra-native primitives in Thetis ports)
// ============================================================
//
// This file replaces the earlier C++23-idiom translation of
// ChannelMaster/aamix.{h,c}.  The earlier version was observably
// similar but used Lyra-native primitives (std::atomic,
// std::counting_semaphore, std::binary_semaphore, std::mutex,
// std::jthread, std::vector, std::function).  The retrofit uses
// Win32 primitives directly so the port mirrors the reference
// byte-for-byte:
//
//   * snake_case struct fields verbatim (id, outbound_id, run,
//     accept, ringinsize, outsize, ninputs, rsize, ring, out,
//     active, nactive, what, vol, tvol, volume, inidx, outidx,
//     unqueuedsamps, Ready, Aready, cs_in, cs_out, rsmp, inrate,
//     outrate, resampbuff, Outbound, slew sub-struct fields).
//   * `volatile long` for run/accept[32]/active/what/slew.uflag/
//     slew.dflag; used with `_InterlockedAnd` and
//     `InterlockedBitTestAndSet/Reset` intrinsics (<intrin.h>).
//   * `HANDLE` semaphores via CreateSemaphore (Ready[32] max=1000,
//     uwait/dwait max=1) + WaitForSingleObject /
//     WaitForMultipleObjects / ReleaseSemaphore / CloseHandle.
//   * `CRITICAL_SECTION cs_in[32] / cs_out` with
//     InitializeCriticalSectionAndSpinCount(&cs, 2500) — the 2500
//     spin count preserved verbatim per the reference.
//   * raw `double*` buffers allocated with calloc + freed with
//     free (mirrors the reference's malloc0 + _aligned_free pair).
//   * raw C function-pointer `void (*Outbound)(int, int, double*)`
//     in the struct field (mirrors aamix.h:61).
//   * `paamix[MAX_EXT_AAMIX]` as a raw C array of pointers
//     (NOT std::array<unique_ptr<...>>).
//   * `_beginthread(mix_main, 0, (void*)a)` + `_endthread()` for
//     the mixer thread (from <process.h>).
//   * `AvSetMmThreadCharacteristics("Pro Audio", &taskIndex)` +
//     `AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH)` from
//     <avrt.h> (link avrt.lib).
//   * NO defensive bounds checks, NO `nullptr` returns from
//     accessors that don't have them in the reference.
//
// `complex` is the reference's typedef for a stereo double pair
// (sizeof(complex) == 2 * sizeof(double) = 16 bytes).  In Lyra-cpp
// the bare name collides with std::complex, so the byte size is
// exposed as a constant and used inline.
//
// ALLOWED FORCED DEVIATIONS (mirror ILV retrofit precedent — these
// are the ONLY deviations from byte-faithful porting):
//
//   * Struct tag stays as `AAMix` (Lyra-cpp PascalCase) instead of
//     reference's `aamix` (typedef `aamix, *AAMIX`) — required
//     because external callers (wdsp_engine.cpp, RadioNet.cpp,
//     CMaster.cpp) declare `lyra::wdsp::AAMix*` directly and would
//     need their own retrofit to flip to `aamix*`.  Rule #8
//     caller-protection carve-out.
//
//   * The `Outbound` struct field + the `SetAAudioMixOutputPointer`
//     public setter keep `std::function<void(int,int,double*)>`
//     instead of the reference's raw C function pointer — required
//     because the existing public setter signature already takes a
//     `std::function` (callers in wdsp_engine.cpp + CMaster.cpp
//     pass lambdas with captures).  Rule #8 caller-protection
//     carve-out.  Note this differs from `mix_main`'s internal
//     invocation pattern (still a direct call of the field) so the
//     dispatch logic mirrors the reference byte-for-byte; only the
//     storage type differs.
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.  See docs/RULES.md AA. 2026-06-08 Amendment for
// the project-level rule change that permits this port.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <functional>

namespace lyra::dsp { class WdspNative; }

namespace lyra::wdsp {

// Reference aamix.c:29 — `#define MAX_EXT_AAMIX (4)`.  Process-wide
// central bank size.
inline constexpr int MAX_EXT_AAMIX = 4;

// Reference aamix.h:40 — fixed compile-time upper bound on the
// number of inputs per mixer.  Reference uses bare `[32]` arrays
// throughout; Lyra-cpp lifts the literal into a named constant so
// the array sizing reads consistently.
inline constexpr int AAMIX_MAX_INPUTS = 32;

// `complex` in the reference is `typedef double complex[2]`; the
// byte size used by malloc0/memcpy/memset is exposed by ILV.h in
// the same namespace as `kSizeofComplex` (identical definition;
// not redeclared here to avoid an MSVC C2374 against the sibling
// retrofit header).

// Reference inner `slew` anonymous struct at aamix.h:62-84.
// Promoted to a named struct in Lyra-cpp purely for clearer member
// access; field set + types + defaults match reference byte-for-byte
// (with the two allowed forced deviations documented at the file
// header).
struct AaSlew {
    // Reference aamix.h:64-67 — operator-supplied slew timing (s).
    double tdelayup;          // delay before upslew
    double tslewup;            // upslew time
    double tdelaydown;        // delay before downslew
    double tslewdown;         // downslew time

    // Reference aamix.h:68-73 — state-machine counters / state.
    int ustate;                // state of upslew function
    int dstate;                // state of downslew function
    int ucount;                // counter for upslew function
    int dcount;                // counter for downslew function
    int ndelup;                // number of samples for delayup time
    int ntup;                  // number of samples for upslew
    double* cup;               // upslew coefficients
    int ndeldown;              // number of samples for delaydown time
    int ntdown;                // number of samples for downslew
    double* cdown;             // coefficients for downslew

    // Reference aamix.h:78-79 — set when slew is to proceed or in
    // progress.  Bit 0 of each is the active flag; manipulated via
    // _InterlockedAnd / InterlockedBitTestAndSet / InterlockedBit
    // TestAndReset intrinsics.
    volatile long uflag;
    volatile long dflag;

    // Reference aamix.h:80-81 — semaphore handles the close/open
    // callers block on until the corresponding slew completes.
    // CreateSemaphore(0, 0, 1, 0) at aamix.c:100-101.
    HANDLE uwait;
    HANDLE dwait;

    // Reference aamix.h:82-83 — operator timeouts (ms), computed
    // in create_aaslew from t{delay,slew}{up,down}; aamix.c:105-106.
    int dtimeout;
    int utimeout;
};

// Reference `struct _aamix` typedef at aamix.h:32-85.
//
// Field names + types are verbatim from the reference (snake_case,
// volatile long, HANDLE, CRITICAL_SECTION, raw double*) with the
// two allowed forced deviations.
struct AAMix {
    // Reference aamix.h:34-35.
    int id;                    // id of this aamixer
    int outbound_id;           // id to use in the Outbound() call

    // Reference aamix.h:36 — thread runs when set to 1.
    volatile long run;

    // Reference aamix.h:37 — ring accepts data when set to 1.  One
    // bit-0 flag per input slot, manipulated via Interlocked
    // intrinsics.
    volatile long accept[AAMIX_MAX_INPUTS];

    // Reference aamix.h:38-41.
    int ringinsize;            // input size to rings, complex samples
    int outsize;               // size to output, complex samples
    int ninputs;               // number of inputs, assumed consecutive
                               // beginning at 0
    int rsize;                 // total size of a ring, complex samples

    // Reference aamix.h:42 — per-input ring buffers (interleaved IQ),
    // sized `rsize * sizeof(complex)` and allocated via malloc0 at
    // aamix.c:165.
    double* ring[AAMIX_MAX_INPUTS];

    // Reference aamix.h:43 — pointer to output buffer (interleaved
    // IQ), sized `outsize * sizeof(complex)`, allocated via malloc0
    // at aamix.c:166.
    double* out;

    // Reference aamix.h:44-46.
    volatile long active;      // one bit per active (data flowing) input
    int nactive;               // number of active inputs
    volatile long what;        // one bit per item to mix

    // Reference aamix.h:47-49.
    double vol [AAMIX_MAX_INPUTS];   // volume scaling per input
    double tvol[AAMIX_MAX_INPUTS];   // final volume scaling
    double volume;                   // master volume scaling

    // Reference aamix.h:50-52.
    int inidx        [AAMIX_MAX_INPUTS];   // input  indexes of rings
    int outidx       [AAMIX_MAX_INPUTS];   // output indexes of rings
    int unqueuedsamps[AAMIX_MAX_INPUTS];   // samples not yet released

    // Reference aamix.h:53-54.
    HANDLE Ready [AAMIX_MAX_INPUTS];   // semaphore handles, one per
                                       // possible input
    HANDLE Aready[AAMIX_MAX_INPUTS];   // semaphore handles for active
                                       // inputs (subset of Ready[],
                                       // packed for WaitForMultiple
                                       // Objects)

    // Reference aamix.h:55-56.
    CRITICAL_SECTION cs_in[AAMIX_MAX_INPUTS];   // protect input process
    CRITICAL_SECTION cs_out;                    // protect output process

    // Reference aamix.h:57-60.  The reference `RESAMPLE rsmp[32]`
    // type is an opaque pointer to the WDSP resample struct; Lyra-
    // cpp consumes wdsp.dll dynamically and types the slot as
    // `void*`.  Field name + indexing preserved.
    void* rsmp[AAMIX_MAX_INPUTS];      // array of resampler pointers
    int inrate[AAMIX_MAX_INPUTS];      // sample rates of the inputs
    int outrate;                       // sample rate of the output
    double* resampbuff[AAMIX_MAX_INPUTS]; // buffers for resampler outputs

    // Reference aamix.h:61 — `void (*Outbound)(int id, int nsamples,
    // double* buff)`.  ALLOWED FORCED DEVIATION: kept as
    // std::function because the public setter signature already
    // takes std::function (existing call sites pass lambdas with
    // captures).  See file header for rationale.
    std::function<void(int id, int nsamples, double* buff)> Outbound;

    // Reference aamix.h:62-84.
    AaSlew slew;

    // [Lyra-native — single-additional-field deviation] DLL handle
    // for the resampler API.  Required because Lyra-cpp consumes
    // wdsp.dll via dynamic LoadLibrary + GetProcAddress (vs the
    // reference's static link), so the function pointers are not
    // file-scope globals — they are members of the WdspNative
    // singleton.  Set by create_aamix; never reassigned.
    lyra::dsp::WdspNative* wdsp;

    // [Lyra-native — single-additional-field deviation] mix_main
    // thread handle returned by _beginthread (matches reference
    // start_mixthread at aamix.c:51-55 which discards the HANDLE).
    // Stored here so destroy_aamix can pair its run-bit-clear +
    // Ready-semaphore-release sequence with a deterministic teardown
    // observable from outside (the reference relies on Sleep(2) at
    // aamix.c:218 to give the thread time to exit; Lyra-cpp keeps
    // the same Sleep(2) for byte-faithful behavior).
    HANDLE mix_thread;
};

// Reference aamix.c:30 — `__declspec(align(16)) AAMIX paamix[
// MAX_EXT_AAMIX]`.  Process-wide central bank of pointers used
// EXTERNAL to wdsp.  Exposed as a raw C array of pointers to match
// the reference indexing pattern byte-for-byte.
extern AAMix* paamix[MAX_EXT_AAMIX];

// ===== Public API (matches aamix.h:87-122 byte-for-byte) =====
//
// The `void* ptr` parameter pattern (ptr=0 → look up AAMix via
// paamix[id]) is preserved verbatim.  In Lyra-cpp the pointer type
// is the typed `AAMix*` rather than `void*` — callers can always
// pass `nullptr` to trigger the id-based look-up.
//
// `__declspec(dllexport)` annotations from the reference are
// dropped because Lyra-cpp's AAMix is consumed in-process.

// Reference aamix.h:87-104 — `create_aamix(...)` constructor.
//
// **[Lyra-native]** extra leading param `wdsp` — the resolved
// WdspNative singleton, so create_aamix + subsequent operations
// can reach the resample API without a global lookup.  Single-
// param divergence from reference; mixer semantics unchanged.
//
// The `Outbound` parameter keeps std::function for caller-protection
// (rule #8 carve-out — see file header).
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

// Reference aamix.h:106.
void destroy_aamix(AAMix* ptr, int id);

// Reference aamix.h:108.  Public for unit-test access.
void xaamix(AAMix* a);

// Reference aamix.h:110.
void xMixAudio(AAMix* ptr, int id, int stream, double* data);

// Reference aamix.h:112.  ALLOWED FORCED DEVIATION: keeps
// std::function — see file header rationale.
void SetAAudioMixOutputPointer(
    AAMix* ptr, int id,
    std::function<void(int id, int nsamples, double* buff)> Outbound);

// Reference aamix.h:114-122.
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
