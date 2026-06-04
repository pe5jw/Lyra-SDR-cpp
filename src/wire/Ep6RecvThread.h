// Lyra — EP6 receive thread (§3 / §10.2 wire layer).
//
// std::thread, MMCSS Pro Audio.  recvfrom loop on EP6 socket;
// parses each datagram, dispatches via `DdcMap` to per-stream
// rings.  Mirrors the reference `MetisReadThreadMain`.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3 / §8.

#pragma once

namespace lyra::wire {

class Ep6RecvThread {
public:
    Ep6RecvThread();
    ~Ep6RecvThread();
};

}  // namespace lyra::wire
