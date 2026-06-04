// Lyra — PTT funnel FSM (§5 / §10.2 tx layer).
//
// Single-funnel FSM, `PttSource` enum {SwMox, HwPtt, Cat, Tci, Vox,
// CwKey, Tun}, source set, resolver.  Mirrors
// `chkMOX_CheckedChanged2 + single shared MOX state`.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.

#pragma once

namespace lyra::tx {

class PttFsm {
public:
    PttFsm();
    ~PttFsm();
};

}  // namespace lyra::tx
