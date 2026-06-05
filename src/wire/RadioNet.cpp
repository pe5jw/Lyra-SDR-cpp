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
// `XmitBit` mirrors the reference name verbatim (network.h:413).
// Zero at startup; written from the FSM's MOX-edge code in Phase
// 2 wire-up.  `volatile long` → `std::atomic<long>` idiom
// translation locked.
//
// `hpsdrModel` defaults to `HERMESLITE` (HL2 / HL2+ — the current
// Lyra target); replaced at session start by discovery + the
// per-family capability lookup when ANAN / Atlas / Saturn tester
// hardware arrives.  `radioProtocol` defaults to `USB` (Protocol
// 1 — HL2 is P1).  Both variables are renamed from the reference's
// C-style enum-name shadow per §3.4 (acceptable deviation; role
// preserved).
std::atomic<long> XmitBit{0};
HPSDRModel        hpsdrModel    = HPSDRModel::HERMESLITE;
RadioProtocol     radioProtocol = RadioProtocol::USB;

}  // namespace lyra::wire
