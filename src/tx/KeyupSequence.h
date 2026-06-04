// Lyra — Keyup sequence (§5 / §10.2 tx layer).
//
// Exact 16-step keyup order.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.

#pragma once

namespace lyra::tx {

class KeyupSequence {
public:
    KeyupSequence();
    ~KeyupSequence();
};

}  // namespace lyra::tx
