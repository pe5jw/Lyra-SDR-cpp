// Lyra — C&C frame composer (§8 / §10.2 wire layer).
//
// Builds the 19-case C&C round-robin (cases 0-18) mirroring the
// reference `WriteMainLoop_HL2` for HL2/HL2+ behavior shipped
// first; generic-P1 (Hermes/Angelia/Orion/older-ANAN) and P2
// branches land when tester hardware is available (assert-on-
// hit until then per Q2 family-parameterized lock).  Reads from
// `RadioNet` + `RbpFilter`/`RbpFilter2` + `DispatchState`;
// emits 8-byte C&C headers into EP2 frames assembled by
// `Ep2SendThread`.  Per-case dispatch axes (MOX / PS-armed /
// active radio model / active wire protocol) read from the
// reference-verbatim globals at the use site per signed §3
// checkpoint (`XmitBit`, `prn->puresignal_run`, `hpsdrModel`,
// `radioProtocol`).
//
// ALL bits source-verified per Phase 0 §8.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §8.

#pragma once

namespace lyra::wire {

class FrameComposer {
public:
    FrameComposer();
    ~FrameComposer();
};

}  // namespace lyra::wire
