// Lyra — Keydown sequence (§5 / §10.2 tx layer).
//
// Exact 16-step keydown order.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.

#pragma once

namespace lyra::tx {

class KeydownSequence {
public:
    KeydownSequence();
    ~KeydownSequence();
};

}  // namespace lyra::tx
