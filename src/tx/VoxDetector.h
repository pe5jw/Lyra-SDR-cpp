// Lyra — VOX gate (voice-operated TX), #91.
//
// Pure, hardware-free decision core: fed the running mic RMS and (for
// anti-VOX) the running RX-audio RMS, it decides whether TX should be
// keyed.  No Qt, no threads, no I/O — unit-testable in isolation, the
// same posture as CwMorse / CwDecoder.  The HL2Stream integration owns
// the sample taps (EP6 mic + RX dispatchAudioFrame), the mode/inhibit
// gating, and the funnel into requestMox(…, PttSource::Vox); this class
// owns ONLY the threshold + open-delay + hang + anti-VOX timing.
//
// Behaviour (classic VOX):
//   - CLOSED: the mic must stay above `thresholdDbfs` for `openMs`
//     before opening (open-delay rejects clicks/thumps).  While RX
//     audio is above `antiVoxDbfs`, opening is SUPPRESSED (anti-VOX —
//     the operator's own monitor/speaker audio can't key the rig).
//   - OPEN: mark-above refreshes the `hangMs` hold so inter-word gaps
//     don't drop TX; once the mic falls quiet the hang counts down and
//     the gate closes.  Anti-VOX does NOT drop an already-open key —
//     you're transmitting, the receiver is muted, so the RX level is
//     moot until the next open decision.
//
// Levels are dBFS (0 dB = full scale).  Fed linear RMS in [0, 1]; the
// class converts internally.
//
// Reference (GPL v3+ direct port, open attribution): Thetis
// Console/console.cs `PollPTT()` + `PTTMode` — VOX keys as
// `vox_ptt = vox_ok && Audio.VOXActive` (mic un-muted AND the mic-level
// gate fired), returns `PTTMode.VOX` only in voice/digital/FM modes
// (NOT CW), and the whole auto-PTT poll is suppressed while
// `_manual_mox || _tx_inhibit || _rx_only || QSKEnabled`.  This class
// is the Lyra-native mic-gate half (VOXActive-equivalent); the HL2Stream
// layer owns the mode / mic-mute / inhibit / manual-key gating + the
// requestMox(…, PttSource::Vox) funnel.  Kept self-contained (a plain
// RMS gate, not the WDSP DEXP the reference reads) so VOX adds nothing
// to the TX DSP chain — PureSignal-safe: VOX only decides WHEN to key.

#pragma once

namespace lyra::tx {

class VoxDetector {
public:
    struct Params {
        double thresholdDbfs = -35.0;  // mic must exceed this to open
        double antiVoxDbfs   = -45.0;  // RX above this suppresses opening
        int    openMs        = 10;     // mic must hold above threshold this long
        int    hangMs        = 300;    // TX held this long after the mic drops
        bool   antiVoxOn     = true;   // enable the anti-VOX suppression
    };

    void setParams(const Params& p) noexcept { p_ = p; }
    const Params& params() const noexcept { return p_; }

    // Force the gate closed and clear the open/hang accumulators.  Call
    // when VOX is disabled, on a mode change out of voice, or when
    // another PTT source takes the key (so VOX starts clean next time).
    void reset() noexcept;

    // Advance the gate by `dtMs` milliseconds given the current linear
    // RMS of the mic and the RX audio.  Returns the desired key state
    // (true = TX).  Pure — the caller acts on edges of the return value.
    bool tick(double micRmsLin, double rxRmsLin, double dtMs) noexcept;

    bool keyed() const noexcept { return keyed_; }

private:
    Params p_{};
    bool   keyed_   = false;
    double openAcc_ = 0.0;   // ms mic has been above threshold (pre-open)
    double hangAcc_ = 0.0;   // ms remaining in the hang hold
};

}  // namespace lyra::tx
