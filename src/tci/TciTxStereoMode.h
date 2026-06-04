// Lyra — TCI TX stereo input mode (§7 / §10.2 tci layer).
//
// Header-only enum class shell.  Phase 2 populates the enumerators
// (Both / Left / Right) with `Both` as default.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §7.

#pragma once

namespace lyra::tci {

enum class TciTxStereoMode {
    // Enumerators populated in Phase 2 (Both / Left / Right).
};

}  // namespace lyra::tci
