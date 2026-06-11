// Lyra-cpp — CMaster.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/cmaster.h: cmaster typedef + pcm global
//   - ChannelMaster/cmcomm.h:  struct _cmaster + xmtr substruct
//   - ChannelMaster/cmaster.c: create_cmaster / destroy_cmaster /
//                              create_xmtr / xcmaster body /
//                              SendpOutbound* setters
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014-2019 Warren Pratt, NR0V
//                     and the openHPSDR / Thetis contributors
// License: GNU General Public License v3 or later
// Lyra-cpp is also GPL v3+; redistribution complies with GPL
// terms (preserved copyright, documented modifications, complete
// source available at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT + LYRA-NATIVE create_xmtr_hl2
// SPLIT REVERTED 2026-06-09
// (operator directive: no Lyra-native deviations in Thetis ports)
// ============================================================
//
// This file replaces the earlier multi-stage shell that had a
// thin `CMasterState` plus a Lyra-native `pxmtr[]` bank populated
// by `create_xmtr_hl2`.  The retrofit restructures `CMasterState`
// to absorb the reference `_cmaster` struct fields the ported
// functions actually use, ports `create_xmtr()` byte-faithful per
// `cmaster.c:112-253`, and routes xcmaster's TX case 1 through
// `pcm->xmtr[tx].pilv` + `pcm->xmtr[tx].pTxChannel` exactly the
// way the reference reaches `pcm->xmtr[tx].pilv`.
//
// ALLOWED FORCED DEVIATIONS (rule-#8 caller-protection + the
// single Lyra-native carve-out the operator approved):
//
//   * Struct tag stays as `CMasterState` (Lyra-cpp PascalCase)
//     instead of the reference's `cmaster` typedef -- external
//     callers declare `lyra::wire::CMasterState*` and would need
//     their own retrofit to flip.  Rule #8 caller-protection.
//
//   * Outbound* / Inbound* function-pointer fields stay as the
//     `OutboundCallback`/`InboundCallback` std::function typedefs
//     because the existing public setter signatures already take
//     std::function (CMaster.cpp's SendpOutbound* + downstream
//     callers in wdsp_engine.cpp + RadioNet.cpp pass lambdas with
//     captures).  Rule #8 caller-protection.  The struct fields
//     match the setter types byte-for-byte so the value stored is
//     the value dispatched -- no thunk, no parallel storage.
//
//   * (RESOLVED P0.b 2026-06-11) The xmtr substruct's `pilv` is now
//     the reference `ILV` typedef verbatim (src/wire/ILV.h direct
//     port), and `OutboundTx` / `SendpOutboundTx` carry the
//     reference's raw `void(*)(int,int,double*)` — the former
//     "rule-#8" std::function deviation on the TX side is gone.
//

//   * One Lyra-cpp carve-out: `pTxChannel` field in the xmtr
//     substruct.  Operator-acknowledged: Lyra-cpp's `TxChannel` is
//     a C++23 RAII class constructed in main.cpp (vs the reference
//     opening the WDSP channel INSIDE create_xmtr), because
//     Lyra-cpp's runtime-loaded WDSP.dll forces TxChannel
//     construction to defer until WDSP DLL symbols resolve.  The
//     field-level extension is the minimal seam: main.cpp directly
//     assigns `pcm->xmtr[0].pTxChannel = txChannel;` instead of
//     calling a function -- mirrors how the reference would have
//     held the equivalent pointer inside its xmtr struct if it
//     were RAII-constructed externally.
//
// See NOTICE.md and CREDITS.md (repo root) for full attribution.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <functional>

#include "wire/ILV.h"   // P0.b direct port — the reference `ilv, *ILV` typedef

// Forward decls -- full defs in src/wdsp/ + src/wire/CmBuffs.h.
// Kept out of this header's #includes to avoid pulling the WDSP
// cffi surface into every wire-layer translation unit.
namespace lyra::wdsp { class TxChannel; }
namespace lyra::wire { struct CmBuffs; }

namespace lyra::wire {

// ===== Audio codec selector =====
//
// Reference `enum AudioCODEC` at cmaster.h:116-121.  Selects whether
// the radio's onboard codec (HERMES -- the HL2/HL2+ AK4951 path) or
// the host's audio device (ASIO / WASAPI) drives RX audio output.
// `audio_codec_id` mirrors `pcm->audioCodecId` at cmaster.h:70.
//
// Default `HERMES` matches HL2/HL2+ operating point (onboard codec
// drives RX audio via EP2).  ASIO requires the `cmasio.c` port;
// WASAPI is "not implemented" in the reference as well.
enum class AudioCodecId : int {
    HERMES = 0,   // HL2/HL2+ onboard codec via EP2 -- Lyra default
    ASIO   = 1,   // host ASIO sound device (cmasio.c port pending)
    WASAPI = 2,   // host WASAPI -- reference comments "not implmented"
};

// ===== Outbound / Inbound callback signatures =====
//
// Reference function pointer signatures at cmaster.h:65-68.
//
// Reference C signatures (kept here for grep-parity):
//   void (*OutboundRx)(int id, int nsamples, double* buff)
//   void (*OutboundTx)(int id, int nsamples, double* buff)
//   void (*OutboundTCIRxIQ)(int id, int nsamples, double* buff)
//   void (*InboundTCITxAudio)(int nsamples, double* buff)
//
// Lyra uses std::function per the rule-#8 caller-protection
// carve-out -- existing setters take std::function with captures.
using OutboundCallback = std::function<void(int id, int nsamples, double* buff)>;
using InboundCallback  = std::function<void(int nsamples, double* buff)>;

// ===== Compile-time size constants =====
//
// Reference cmaster.h/cmcomm.h compile-time limits.  Lyra-cpp v0.2
// uses HL2 SSB with one transmitter and the stream-1 (TX) CMB ring;
// constants sized for the reference HL2 nddc=4 layout (5 streams:
// RX1/TX/RX2/RX-aux/RX-sub).  Match reference values so array
// indexing patterns port byte-for-byte.
inline constexpr int cmMAXstream  = 5;
inline constexpr int cmMAXxmtr    = 1;

// ===== Per-transmitter substruct (reference cmcomm.h xmtr[]) =====
//
// Reference cmcomm.h:85-97 -- the `struct _xmtr` substruct inside
// `_cmaster`.  Lyra-cpp ports only the fields the HL2 SSB path
// actually uses; the deferred-by-design subsystems (DEXP/AAMIX
// anti-VOX/TXGAIN/EER/sidetone) are reference-`run=0` for HL2 or
// scoped to later releases per Stage E.0 audit
// (docs/architecture/STAGE_E_TXCHANNEL_AUDIT.md).
//
// One Lyra-native carve-out: `pTxChannel`.  Reference opens the
// WDSP TXA channel INSIDE create_xmtr (cmaster.c:177-190).
// Lyra-cpp's TxChannel is a C++23 RAII class constructed in
// main.cpp because WDSP.dll loads at runtime; main.cpp publishes
// the constructed pointer here via direct assignment, which is
// the minimal Lyra-native seam (a function call would itself be
// a deviation from "field-level access mirrors the reference").
struct CMasterXmtr
{
    int ch_outrate = 0;  // xmtr output rate = TX channel output rate
    int ch_outsize = 0;  // xmtr output size = TX channel output size

    // Reference's per-xmtr ILV pointer (cmcomm.h:94 `ILV pilv;`).
    // Populated by create_xmtr's create_ilv call (cmaster.c:226-232);
    // dispatched by xcmaster case 1 (cmaster.c:397
    // `xilv(pcm->xmtr[tx].pilv, pcm->xmtr[tx].out);`).
    // P0.b: `ILV` is the reference twin typedef (= `ilv*`) from the
    // verbatim src/wire/ILV.h direct port.
    ILV pilv = nullptr;

    // Reference cmcomm.h:96 `volatile long use_tci_audio;`.
    // Set via SetTXTCIAudio(); read at xcmaster case-1 TCI dispatch
    // (cmaster.c:380).  Plain `volatile long` per the byte-faithful
    // retrofit posture; cross-thread updates use Win32 intrinsics.
    volatile long use_tci_audio = 0;

    // [Lyra-native carve-out] The TxChannel pointer main.cpp
    // constructs after WDSP.dll loads.  Reference would have opened
    // the WDSP TXA channel inline inside create_xmtr; Lyra-cpp's
    // runtime-loaded DLL forces external construction, and
    // publishing the pointer via a direct field assignment is the
    // minimal seam.  xcmaster case 1 dispatches through this
    // pointer the same way the reference dispatches via
    // pcm->xmtr[i].out[0] (it's the channel-output indirection).
    lyra::wdsp::TxChannel* pTxChannel = nullptr;
};

// ===== Global cmaster state (reference cmcomm.h _cmaster struct)
// =====
//
// Reference `cmaster cm = {0}; CMASTER pcm = &cm;` at
// cmaster.c:29-30.  Process-lifetime; never freed.
//
// Lyra-cpp populates only the fields the ported functions touch
// (reference struct has many more for the deferred-by-design
// subsystems -- ANB/NOB/audio/aamix_inrates/panalalloc/rcvr[]/
// xmtr substruct fields beyond pilv/use_tci_audio).  Each added
// field cites its reference cmcomm.h line for audit.
struct CMasterState
{
    // Reference cmaster.h:70 `int audioCodecId;`
    AudioCodecId audioCodecId = AudioCodecId::HERMES;

    // Reference cmcomm.h:54 `double *in[cmMAXstream];`.
    // Per-stream xcmaster() input buffer.  Reference allocates in
    // create_cmaster (cmaster.c:285).  Owned by CmBuffs.cpp's
    // per-stream storage; this slot is a non-owning view exposed
    // for `pcm->in[id]` indexing in xcmaster's case 1
    // (cmaster.c:389 `fexchange0(... pcm->in[stream] ...)`).
    double* in[cmMAXstream] = { nullptr, nullptr, nullptr,
                                nullptr, nullptr };

    // Reference cmcomm.h:59-62 -- four `CMB` aliases per stream all
    // pointing to the SAME cmbuffs slot (create_cmbuffs cmbuffs.c:38
    // sets all four).  Lyra-cpp keeps the four-alias indexing
    // surface so cmbuffs.c-ported sites read `pcm->pcbuff[id]`,
    // `pcm->pdbuff[id]`, etc., byte-for-byte.  All four point at
    // the same CmBuffs instance per stream.
    CmBuffs* pcbuff[cmMAXstream] = { nullptr, nullptr, nullptr,
                                     nullptr, nullptr };
    CmBuffs* pdbuff[cmMAXstream] = { nullptr, nullptr, nullptr,
                                     nullptr, nullptr };
    CmBuffs* pebuff[cmMAXstream] = { nullptr, nullptr, nullptr,
                                     nullptr, nullptr };
    CmBuffs* pfbuff[cmMAXstream] = { nullptr, nullptr, nullptr,
                                     nullptr, nullptr };

    // Reference cmcomm.h:97 `struct _xmtr xmtr[cmMAXxmtr];`.
    CMasterXmtr xmtr[cmMAXxmtr];

    // Reference cmaster.h:65-68 -- the 4 callback function pointers.
    // Stored by SendpOutbound* setters; wired into AAMix (RX) and
    // ILV (TX) downstream so the callbacks fire on every frame
    // those modules produce.  Rule-#8 carve-out: std::function so
    // existing public setters can pass capturing lambdas.
    OutboundCallback OutboundRx;
    // P0.b: the TX outbound is the reference raw function pointer
    // VERBATIM (cmaster.h:66) — it is stored into ILV's `Outbound`
    // field by create_ilv / SetILVOutputPointer, so it must be the
    // exact reference type.  RX-side callbacks convert in P0.c/P0.d.
    void (*OutboundTx)(int id, int nsamples, double* buff) = nullptr;
    OutboundCallback OutboundTCIRxIQ;
    InboundCallback  InboundTCITxAudio;

    // Reference cmaster.h:69 `volatile long tci_run;`.
    // Set via SetTCIRun(); read at TCI dispatch sites.  Plain
    // `volatile long` per the byte-faithful retrofit; cross-thread
    // updates use Win32 intrinsics.
    volatile long tci_run = 0;
};

extern CMasterState  cm;
extern CMasterState* pcm;

// ===== SendpOutbound* / SendpInbound* setters =====
//
// Reference cmaster.c:407-432.  Each setter stores the callback
// into `pcm->Outbound*` / `pcm->Inbound*` AND in the reference's
// HERMES / TX path additionally wires the callback into the
// downstream module (AAMIX for RX, ILV for TX).
//
// Reference C signatures preserved verbatim for grep-parity:
//   void SendpOutboundRx (void (*Outbound)(int id, int nsamples, double* buff))
//   void SendpOutboundTx (void (*Outbound)(int id, int nsamples, double* buff))
//   void SendpOutboundTCIRxIQ (void (*Outbound)(int id, int nsamples, double* buff))
//   void SendpInboundTCITxAudio (void (*Inbound)(int nsamples, double* buff))
void SendpOutboundRx(OutboundCallback cb);
// P0.b: verbatim reference signature (cmaster.c:414) — the stored
// pointer flows straight into ILV's raw `Outbound` field.  NOTE the
// verbatim body has NO null-pilv guard (reference cmaster.c:418 →
// ilv.c:97-101): callers must register AFTER create_xmtr has
// populated pcm->xmtr[0].pilv, exactly as the reference orders it.
void SendpOutboundTx(void (*Outbound)(int id, int nsamples, double* buff));
void SendpOutboundTCIRxIQ(OutboundCallback cb);
void SendpInboundTCITxAudio(InboundCallback cb);

// ===== SetTCIRun (reference cmaster.c:434-437) =====
void SetTCIRun(int active);

// =====================================================================
// Reference cmaster.c:273-337 -- process-wide CMaster lifecycle.
// =====================================================================
//
// MUST be called ONCE at app startup BEFORE any consumer touches
// the cmaster.  Mirrors reference `create_cmaster` ordering
// (cmaster.c:273-320).
void create_cmaster();

// Reverse-order teardown.  MUST be called ONCE at app shutdown
// AFTER all consumers have been torn down.  Mirrors reference
// `destroy_cmaster` (cmaster.c:322-337).
void destroy_cmaster();

// =====================================================================
// Reference cmaster.c:112-253 -- create_xmtr / destroy_xmtr.
// =====================================================================
//
// Ports the HL2 SSB minimum surface per Stage E.0 audit
// (docs/architecture/STAGE_E_TXCHANNEL_AUDIT.md): out buffers (via
// TxChannel RAII) + OpenChannel (via TxChannel) + ILV.  The 5
// deferred subsystems (DEXP / anti-VOX / TXGAIN / EER / sidetone)
// are reference-`run=0` for HL2 or deferred-by-design.  TxChannel
// lifecycle is owned by main.cpp (RAII for runtime-loaded WDSP.dll
// reasons); create_xmtr expects main.cpp has already published
// `pcm->xmtr[i].pTxChannel` before invoking, otherwise the ILV's
// `insize` cannot be derived from the TxChannel's `outSize()`.
void create_xmtr(int xmtr_id);

// Tears down the ILV slot for `xmtr_id`.  Mirrors reference
// destroy_xmtr (cmaster.c:255-271) reduced to the surfaces
// create_xmtr actually creates.  TxChannel lifecycle stays owned
// by main.cpp.
void destroy_xmtr(int xmtr_id);

// =====================================================================
// xcmaster -- reference cmaster.c:340-405.
// =====================================================================
//
// Reference `PORT void xcmaster(int stream)`.  Dispatches per-stream
// type: case 0 = standard receiver, case 1 = standard transmitter,
// case 2 = special upper-panadapter stitch.
//
// Lyra-cpp v0.2 HL2 SSB:
//   * case 1 is the live path -- ports the reference body reduced
//     to the HL2 SSB surfaces per Stage E.0 audit
//     (fexchange0 via TxChannel::process + xilv via pcm->xmtr[tx].
//     pilv).
//   * case 0 is a documented stub (Lyra dispatches RX via WdspEngine
//     in its pre-existing architecture).
//   * case 2 is a documented stub (special-panadapter stitch
//     unused).
//
// xcmaster is the consumer-facing entry the cm_main pump invokes
// once per drained CMB-ring output block (cmbuffs.c:165).
void xcmaster(int stream);

}  // namespace lyra::wire
