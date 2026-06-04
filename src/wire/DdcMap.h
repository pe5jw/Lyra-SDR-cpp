// Lyra — DDC routing matrix (§5.4 / §10.2 wire layer).
//
// Given an `Hl2DispatchState`, returns which DDC samples route to
// which consumer (RX1, RX2, PureSignal feedback, drop) per the
// reference UpdateDDCs + cntrl1 routing matrix.  HL2 nddc=4.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.4.

#pragma once

namespace lyra::wire {

class DdcMap {
public:
    DdcMap();
    ~DdcMap();
};

}  // namespace lyra::wire
