// Lyra — DSP stream ring (§3 / §10.2 dsp layer).
//
// SPSC ring buffer, kernel-semaphore-paired, complex64 elements,
// capacity `CMB_MULT × outsize`.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3.

#pragma once

namespace lyra::dsp {

class StreamRing {
    // Slot type / capacity / semaphore pair populated in Phase 2.
};

}  // namespace lyra::dsp
