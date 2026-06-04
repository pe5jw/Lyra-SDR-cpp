// Lyra — TCI TX audio bridge (§7 / §10.2 tci layer).
//
// queueTciTxAudio + OnTciTxAudioInSamples bridge.  L+R averaging
// per TXStereoInputMode.  WDSP xresampleFV resample.  Underrun →
// zero-fill.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §7.

#pragma once

namespace lyra::tci {

class TciTxBridge {
public:
    TciTxBridge();
    ~TciTxBridge();
};

}  // namespace lyra::tci
