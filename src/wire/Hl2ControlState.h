// Lyra — HL2 C&C register state (§8 / §10.2 wire layer).
//
// Holds the `prn->*` register fields (drive_level, step_attn for
// ADC[0..2], pa, mic_*, BPF/LPF flags, puresignal_run,
// tx[0].frequency, rx[0..1].frequency, ptt_hang, tx_latency,
// reset_on_disconnect, etc.) used by `Hl2FrameComposer` to build
// each round-robin C&C frame.  Single writer per field — caller
// discipline (Rule 1).
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §8.

#pragma once

namespace lyra::wire {

class Hl2ControlState {
public:
    Hl2ControlState();
    ~Hl2ControlState();
};

}  // namespace lyra::wire
