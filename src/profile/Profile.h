// Lyra — TX/RX operator Profile (Stage-0 model).
//
// A named bundle of operator-facing TX/RX state, recalled as a unit
// (the Thetis TX-Profile idiom, ported Lyra-native — see
// docs/architecture/AUDIO_IO_AND_PROFILES_PLAN.md +
// PROFILE_MODEL_STAGE0_DESIGN.md).  Stage 0 = the EXISTING fields with
// real setters today; EQ / Combinator / monitor are RESERVED
// (added when those features land — schemaVersion gates migration).
//
// Whole-set capture/apply (operator decision 2026-06-14): a profile
// holds the full Stage-0 set; per-field "include" granularity is a
// deferred fast-follow.  PA-enable / HW-PTT-input / space-bar PTT are
// deliberately GLOBAL (safety / input-method), NOT profile fields.
//
// VAC carried (2026-06-14, #158 Stage 4): a profile holds vac1Enabled /
// vac1AutoDigital / vac1RxGainDb / vac1TxGainDb so a digital/VAC profile
// flips source+enable as a unit while a TCI profile keeps tci.  Bench-
// confirmed: VAC TX needs micSource=micpc AND vac1 "on" together (the
// selector arms use_vac_audio; without the engine up the TX inbound cb is
// null → silent).  Devices stay GLOBAL station setup (machine-specific
// routing), NOT profile fields.

#pragma once

#include <QString>
#include <QJsonObject>

namespace lyra::profile {

struct Profile {
    QString name;
    int     schemaVersion = 4;   // v2 (#160): ALC max gain + Leveler trio
                                 // v3 (#49):  + native rack blobs (eq/speech/
                                 //            combinator/plate)
                                 // v4 (#107/#109/#93): + PHROT, FM deviation,
                                 //            CTCSS enable/tone, AM carrier %

    // --- TX/RX bandwidth ---
    // NOTE: the operating mode is deliberately NOT a profile field.
    // Like Thetis TX profiles (database.cs:4304-4540 — ~150 fields, no
    // Mode/DSPMode), a profile is a pure signal chain; recalling one
    // (auto-recall OR explicit Load) never changes your operating mode.
    // Per-family auto-recall (CW/SSB/Digital/AM/SAM/DSB/FM) keys on the
    // mode you switch to but applies only the chain, leaving the mode
    // (and thus your chosen sideband) untouched.
    int     rxBandwidth = 0;   // Hz
    int     txBandwidth = 0;   // Hz
    bool    bwLocked    = false;
    int     filterLow   = 0;   // Hz (shared low edge)

    // --- mic / source / drive ---
    QString micSource = QStringLiteral("mic1");  // mic1 / tci (vac1/vac2 reserved)
    double  micGainDb = 0.0;
    bool    micBoost  = false;
    int     tuneDriveMode = 0;       // #95: 0 slider / 1 tune / 2 fixed
    int     fixedTuneDrivePct = 25;  // #95: TUN level in TuneDriveFixed mode
    // RESERVED — present for JSON round-trip but NOT captured / applied /
    // dirty-tracked.  TX drive level (TX power) + tune-drive % are PER-BAND
    // station settings owned by BandMemory (different power per band /
    // antenna).  Tracking them in a band-agnostic profile made every band
    // change re-apply the band's drive and falsely flag the profile
    // "● modified" (operator report 2026-06-16).  Kept as fields for a
    // possible future "profile carries a default drive" opt-in.
    int     tuneDrivePct = 0;
    int     txDriveLevel = 0;  // 0..255 (the TX-power knob)

    // --- TCI gains ---
    double  tciRxGainDb = 0.0;
    double  tciTxGainDb = 0.0;

    // --- VAC (Virtual Audio Cable, #158) ---
    // Per-profile so a digital/VAC profile carries source(micpc)+enable
    // while a TCI profile keeps tci — flip profiles, not three settings.
    // vac1Enabled MUST ride with micSource=micpc: the mic-source selector
    // arms use_vac_audio, but with VAC1 not "on" the TX inbound cb is null
    // → silent TX.  Devices stay GLOBAL station setup (Settings → Audio),
    // NOT profile fields — machine-specific routing.  Defaults match the
    // engine (rx 0 dB, tx +3 dB).
    bool    vac1Enabled     = false;
    bool    vac1AutoDigital = false;
    double  vac1RxGainDb    = 0.0;
    double  vac1TxGainDb    = 3.0;

    // --- DSP / dynamics ---
    QString agcMode = QStringLiteral("med");     // off/fast/med/slow
    bool    autoMuteOnTx = true;

    // --- ALC / Leveler (§15.27 setters; #160 profile round-trip) ---
    // Operator runs the Leveler ON for SSB, OFF for digital (FT8 etc.) —
    // so its enable+values MUST ride in the profile (operator report
    // 2026-06-15).  Stored as the wire-native LINEAR factors §15.27 uses;
    // defaults mirror HL2Stream (ALC ceiling 3.0, leveler off / max 15.0 /
    // decay 100 ms) so a pre-v2 profile tolerate-loads to sane state.
    double  alcMaxGainLinear     = 3.0;
    bool    levelerOn            = false;
    double  levelerMaxGainLinear = 15.0;
    int     levelerDecayMs       = 100;

    // --- TX modulation knobs (v4: #109 / #107 / #93) ---
    // Part of "how this setup sounds", so they ride per-profile (operator
    // decision 2026-06-20): PHROT on for SSB punch / off for a clean ESSB
    // profile; FM deviation + CTCSS differ between a repeater and a simplex
    // profile; AM carrier % per AM profile.  Defaults mirror HL2Stream
    // (PHROT on, dev 5 kHz wide, CTCSS off / 100 Hz, AM carrier 100 %) so a
    // pre-v4 profile tolerate-loads to the current global behaviour.
    bool    phrotEnabled  = true;
    double  fmDeviationHz = 5000.0;
    bool    ctcssEnabled  = false;
    double  ctcssToneHz   = 100.0;
    double  amCarrierPct  = 100.0;

    // --- TX safety ---
    int     txTimeoutSec    = 600;
    bool    txTimeoutBypass = false;

    // --- native TX DSP rack (#49 v3) ---
    // Each stage serialized as an opaque blob the owning model produces
    // (EqModel / SpeechModel / CombinatorModel / PlateModel saveState()).
    // The Profile struct stays decoupled from the UI models — capture/apply
    // in main.cpp bridges model<->blob.  An empty object means "not stored"
    // (pre-v3 profile, or a stage never captured) → apply leaves the live
    // stage untouched (tolerant loadState).
    QJsonObject eq;
    QJsonObject speech;
    QJsonObject combinator;
    QJsonObject plate;

    QJsonObject toJson() const;
    static Profile fromJson(const QString &name, const QJsonObject &o);

    // Field-equality (name + schemaVersion EXCLUDED — dirty-tracking
    // compares the captured live state against the stored profile's
    // *values*, regardless of which name is active).
    bool sameValues(const Profile &other) const;
};

}  // namespace lyra::profile
