// Lyra — TXA channel wrapper (§6 / §10.2 wdsp layer).
//
// Mirrors `TXA.c create_txa + xtxa`.  bp0 always-on.  ALC always-on.
// sip1 always-on (Rule 10).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §6.

#pragma once

namespace lyra::wdsp {

class TxChannel {
public:
    TxChannel();
    ~TxChannel();
};

}  // namespace lyra::wdsp
