// Lyra — DSP stream registry (§3 / §10.2 dsp layer).
//
// Maps stream-id → StreamThread + StreamRing.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3.

#pragma once

namespace lyra::dsp {

class StreamRegistry {
public:
    StreamRegistry();
    ~StreamRegistry();
};

}  // namespace lyra::dsp
