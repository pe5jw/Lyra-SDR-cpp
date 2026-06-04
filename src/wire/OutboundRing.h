// Lyra — outbound SPSC ring (§3 / §10.2 wire layer).
//
// SPSC ring for LR audio + TX I/Q feeding the EP2 send thread.
// Paired Win32-event / std::condition_variable AND-wait on the
// consumer side (the two-condition wait per §3 / `obbuffs`).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3.

#pragma once

namespace lyra::wire {

class OutboundRing {
    // Slot type / capacity / sync primitives populated in Phase 2.
};

}  // namespace lyra::wire
