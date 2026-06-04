// Lyra — PS finite-state machine (§9 / §10.2 ps layer).
//
// Empty placeholder; v0.3 fills with the operator-facing PS dialog
// states + auto-attenuator state machine.  Inert in v0.2 (Rule 10).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §9.

#pragma once

namespace lyra::ps {

class PsFsm {
public:
    PsFsm();
    ~PsFsm();
};

}  // namespace lyra::ps
