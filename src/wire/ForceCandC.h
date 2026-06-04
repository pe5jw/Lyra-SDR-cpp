// Lyra — startup C&C priming (§8 / §10.2 wire layer).
//
// Sends N priming C&C frames at stream start before the normal
// `Ep2SendThread` loop begins.  Mirrors the reference
// `ForceCandCFrame` priming pass.  Without it, post-priming RX
// freq updates can be missed by HL2 gateware (the §3.2 duplex-bit
// nuance).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §8.

#pragma once

namespace lyra::wire {

class ForceCandC {
public:
    ForceCandC();
    ~ForceCandC();
};

}  // namespace lyra::wire
