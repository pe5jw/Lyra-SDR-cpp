// Lyra — TCI TX supervisor thread (§7 / §10.2 tci layer).
//
// 2 ms poll cadence supervisor thread.  Mirrors TCITxThreadProc.
// The ONLY 2 ms poll in the rip's TX path — supervisory, not
// audio.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §7.

#pragma once

namespace lyra::tci {

class TciTxSupervisorThread {
public:
    TciTxSupervisorThread();
    ~TciTxSupervisorThread();
};

}  // namespace lyra::tci
