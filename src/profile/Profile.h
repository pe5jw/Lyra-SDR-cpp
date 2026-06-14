// Lyra — TX/RX operator Profile (Stage-0 model).
//
// A named bundle of operator-facing TX/RX state, recalled as a unit
// (the Thetis TX-Profile idiom, ported Lyra-native — see
// docs/architecture/AUDIO_IO_AND_PROFILES_PLAN.md +
// PROFILE_MODEL_STAGE0_DESIGN.md).  Stage 0 = the EXISTING fields with
// real setters today; VAC / EQ / Combinator / monitor are RESERVED
// (added when those features land — schemaVersion gates migration).
//
// Whole-set capture/apply (operator decision 2026-06-14): a profile
// holds the full Stage-0 set; per-field "include" granularity is a
// deferred fast-follow.  PA-enable / HW-PTT-input / space-bar PTT are
// deliberately GLOBAL (safety / input-method), NOT profile fields.

#pragma once

#include <QString>
#include <QJsonObject>

namespace lyra::profile {

struct Profile {
    QString name;
    int     schemaVersion = 1;

    // --- TX/RX bandwidth + mode ---
    QString mode;              // e.g. "USB" / "LSB" / "DIGU" ...
    int     rxBandwidth = 0;   // Hz
    int     txBandwidth = 0;   // Hz
    bool    bwLocked    = false;
    int     filterLow   = 0;   // Hz (shared low edge)

    // --- mic / source / drive ---
    QString micSource = QStringLiteral("mic1");  // mic1 / tci (vac1/vac2 reserved)
    double  micGainDb = 0.0;
    bool    micBoost  = false;
    bool    useTuneDrive = false;
    int     tuneDrivePct = 0;
    int     txDriveLevel = 0;  // 0..255 (the TX-power knob)

    // --- TCI gains ---
    double  tciRxGainDb = 0.0;
    double  tciTxGainDb = 0.0;

    // --- DSP / dynamics ---
    QString agcMode = QStringLiteral("med");     // off/fast/med/slow
    bool    autoMuteOnTx = true;

    // --- TX safety ---
    int     txTimeoutSec    = 600;
    bool    txTimeoutBypass = false;

    QJsonObject toJson() const;
    static Profile fromJson(const QString &name, const QJsonObject &o);

    // Field-equality (name + schemaVersion EXCLUDED — dirty-tracking
    // compares the captured live state against the stored profile's
    // *values*, regardless of which name is active).
    bool sameValues(const Profile &other) const;
};

}  // namespace lyra::profile
