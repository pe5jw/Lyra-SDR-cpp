// Lyra — EP2 send thread (§3 / §10.2 wire layer).
//
// std::thread, MMCSS Pro Audio priority 2.  Two-condition AND wait
// for LR + IQ from `OutboundRing`.  MOX-gate-zero on `!XmitBit`.
// CW LSB packing.  Quantize float -> int16.  Per-frame composer
// call (`Hl2FrameComposer`) to assemble C&C header.  Mirrors the
// reference `sendProtocol1Samples`.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3 / §8.

#pragma once

namespace lyra::wire {

class Ep2SendThread {
public:
    Ep2SendThread();
    ~Ep2SendThread();
};

}  // namespace lyra::wire
