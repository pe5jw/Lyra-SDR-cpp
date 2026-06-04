// Lyra — HL2 C&C frame composer (§8 / §10.2 wire layer).
//
// Builds the 19-case C&C round-robin (cases 0-18) mirroring the
// reference `WriteMainLoop_HL2`.  ALL bits source-verified per
// Phase 0 §8.  Reads from `Hl2ControlState` + `Hl2DispatchState`;
// emits 8-byte C&C headers into EP2 frames assembled by
// `Ep2SendThread`.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §8.

#pragma once

namespace lyra::wire {

class Hl2FrameComposer {
public:
    Hl2FrameComposer();
    ~Hl2FrameComposer();
};

}  // namespace lyra::wire
