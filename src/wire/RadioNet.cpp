// Lyra — radio-network state container.  See RadioNet.h.
//
// Phase 2 step 13 — populated per the signed §1 parity checkpoint
// (docs/architecture/PARITY_CHECKPOINTS.md).  WIRE-INERT: built
// but not wired into HL2Stream until §10.3 step 14.

#include "wire/RadioNet.h"

namespace lyra::wire {

// All members carry in-class default initializers, so the
// default ctor/dtor do zero work beyond letting C++ value-init
// each member.  Std::mutex members default-construct via RAII;
// std::atomic<long> defaults to 0; arrays of sub-structs
// value-init via each sub-struct's own in-class defaults.
RadioNet::RadioNet()  = default;
RadioNet::~RadioNet() = default;

// §1.12 — Global instance pointer.  Owned + assigned by the
// wire-layer initializer at HL2 session start (Phase 2 wire-up,
// §10.3 step 14+); stays nullptr until then.  Name mirrors the
// reference's `RADIONET prn` (network.h:291) verbatim per Rule 1
// reference-parity grep discipline.
RadioNet* prn = nullptr;

// §3.4 — Dispatch-relevant runtime globals.
//
// `XmitBit` mirrors the reference verbatim (`network.h:413`).
// Reference type is plain `int` (Rule 24 source-verified
// 2026-06-05); zero at startup; written from the FSM's MOX-edge
// code in Phase 2 wire-up.  No atomic wrapper — reference posture
// preserved.
//
// `hpsdrModel` defaults to `HERMESLITE` (HL2 / HL2+ — the current
// Lyra target); replaced at session start by discovery + the
// per-family init when ANAN / Atlas / Saturn tester hardware
// arrives.  `radioProtocol` defaults to `USB` (Protocol 1 — HL2
// is P1).  Both variables are renamed from the reference's C-style
// enum-name shadow per §3.4 (acceptable deviation; role preserved).
int           XmitBit       = 0;
HPSDRModel    hpsdrModel    = HPSDRModel::HERMESLITE;
RadioProtocol radioProtocol = RadioProtocol::USB;

// §3.5 — Supplemental dispatch globals (`network.h:501-506`).
//
// `nddc` — per-family DDC count.  HL2 / HL2+ default is 4; per-
// family init at session start overwrites for non-HL2 families
// (Hermes II = 2; ANAN 7000DLE = 7; etc.).
//
// `SampleRateIn2Bits` — outbound sample-rate 2-bit code (48k=0,
// 96k=1, 192k=2, 384k=3).  Default 0 = 48k; operator rate setter
// writes per session.
//
// `P1_en_diversity` — diversity-enabled flag (0=off, non-zero=on);
// HL2 has no diversity feature, default 0 stays.
int           nddc              = 4;
unsigned char SampleRateIn2Bits = 0;
int           P1_en_diversity   = 0;

// §3.5 supplement (added 2026-06-05 per §4b-1 source-verification).
// `P1_adc_cntrl` — per-family ADC-to-DDC routing.  HL2 / HL2+ uses
// ADC0 for all DDCs, so default 0 works on the wire (case 4 emits
// C1=0, C2=0).  ANAN models set non-zero values at session open.
int P1_adc_cntrl = 0;

// §4b-2 supplement (added 2026-06-05 per §4b-2 source-verification).
//
// All five globals default 0 per the §15.26-locked PA-OFF-at-startup
// safety.  C2 of frame 0x12 = `0x40` (case 10's OR'd `0b01000000`
// constant only) → gateware `pa_enable = 0` → PA bias OFF → RF
// impossible until operator opt-in (Task #114 Settings → TX →
// "Enable PA").
int xvtr_enable      = 0;
int ApolloFilt       = 0;
int ApolloFiltSelect = 0;
int ApolloTuner      = 0;
int ApolloATU        = 0;

// §5 supplement (added 2026-06-05 per §5 Ep6RecvThread
// source-verification).  Decimation factor 1 = no decimation;
// operator/per-family rate setter overwrites at session open.
int mic_decimation_factor = 1;
int mic_decimation_count  = 0;

}  // namespace lyra::wire
