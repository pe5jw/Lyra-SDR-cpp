// Lyra — iqc lifecycle wrapper (§9 / §10.2 ps layer).
//
// Empty placeholder; wraps SetTXAiqcRun toggles to manage
// RUN/BEGIN/SWAP/END/DONE.  Inert in v0.2 (Rule 10).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §9.

#pragma once

namespace lyra::ps {

class IqcLifecycle {
public:
    IqcLifecycle();
    ~IqcLifecycle();
};

}  // namespace lyra::ps
