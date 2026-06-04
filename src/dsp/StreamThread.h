// Lyra — DSP stream thread (§3 / §10.2 dsp layer).
//
// std::thread, MMCSS Pro Audio @ 2, blocked on counting semaphore.
// One instance per stream.  Mirrors `cm_main` (cmbuffs.c:151).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3.

#pragma once

namespace lyra::dsp {

class StreamThread {
public:
    StreamThread();
    ~StreamThread();
};

}  // namespace lyra::dsp
