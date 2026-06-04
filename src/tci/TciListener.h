// Lyra — TCI per-client listener (§7 / §10.2 tci layer).
//
// Per-client state, `m_txAudioQueue` with drop-OLDEST, per-listener
// handshake state.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §7.

#pragma once

namespace lyra::tci {

class TciListener {
public:
    TciListener();
    ~TciListener();
};

}  // namespace lyra::tci
