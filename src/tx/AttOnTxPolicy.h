// Lyra — ATT-on-TX policy (§5 / §10.2 tx layer).
//
// Save per-band RX1 step-attn on keydown; force tx-att-for-band;
// restore on keyup.  Master default ON.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.

#pragma once

namespace lyra::tx {

class AttOnTxPolicy {
public:
    AttOnTxPolicy();
    ~AttOnTxPolicy();
};

}  // namespace lyra::tx
