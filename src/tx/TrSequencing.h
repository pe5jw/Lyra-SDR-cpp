// Lyra — TR sequencing delays (§5 / §10.2 tx layer).
//
// Header-only struct for the named TR-sequencing delays:
// rf_delay=50, mox_delay=15, space_mox_delay=13, ptt_out_delay=5,
// key_up_delay=10 (operator-tunable defaults).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.

#pragma once

namespace lyra::tx {

struct TrSequencing {
    // Fields (rf_delay / mox_delay / space_mox_delay /
    // ptt_out_delay / key_up_delay) populated in Phase 2.
};

}  // namespace lyra::tx
