// Lyra — HL2 dispatch-axes state (§5 / §10.2 wire layer).
//
// Mirrors the reference `XmitBit` global + the multi-axis state
// {mox, ps_armed, rx2_enabled, family} that gates the wire MOX bit
// + drives the DDC routing matrix.  Read by the wire-send thread
// to gate MOX-bit emission + by `DdcMap` to choose per-DDC
// dispatch targets.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5 / §5.4.

#pragma once

namespace lyra::wire {

struct Hl2DispatchState {
    // Fields populated in Phase 2 (mox / ps_armed / rx2_enabled /
    // family + atomic accessors).
};

}  // namespace lyra::wire
