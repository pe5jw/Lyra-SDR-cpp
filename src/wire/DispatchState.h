// Lyra — dispatch-axes state (§5 / §10.2 wire layer).
//
// Mirrors the reference `XmitBit` global + the multi-axis state
// that gates the wire MOX bit, drives the DDC routing matrix, and
// selects per-family / per-protocol dispatch.  Read by the wire-
// send thread to gate MOX-bit emission + by `DdcMap` to choose
// per-DDC dispatch targets.  Family-parameterized per Q2 lock —
// HL2/HL2+ behavior shipped first, ANAN/Atlas/Saturn paths land
// when their tester hardware is available (assert-on-hit until
// then; type surface ready from day one).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5 / §5.4 and the signed
// §3 parity checkpoint (docs/architecture/PARITY_CHECKPOINTS.md).

#pragma once

namespace lyra::wire {

struct DispatchState {
    // Fields populated in Phase 2 (mox / ps_armed / rx2_enabled /
    // family / protocol + atomic accessors).
};

}  // namespace lyra::wire
