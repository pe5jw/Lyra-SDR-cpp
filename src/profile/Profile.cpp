// Lyra — Profile JSON (de)serialize.  See Profile.h.

#include "profile/Profile.h"

namespace lyra::profile {

QJsonObject Profile::toJson() const {
    QJsonObject o;
    o["schemaVersion"] = schemaVersion;
    o["rxBandwidth"]   = rxBandwidth;
    o["txBandwidth"]   = txBandwidth;
    o["bwLocked"]      = bwLocked;
    o["filterLow"]     = filterLow;
    o["micSource"]     = micSource;
    o["micGainDb"]     = micGainDb;
    o["micBoost"]      = micBoost;
    o["useTuneDrive"]  = useTuneDrive;
    o["tuneDrivePct"]  = tuneDrivePct;
    o["txDriveLevel"]  = txDriveLevel;
    o["tciRxGainDb"]   = tciRxGainDb;
    o["tciTxGainDb"]   = tciTxGainDb;
    o["vac1Enabled"]     = vac1Enabled;
    o["vac1AutoDigital"] = vac1AutoDigital;
    o["vac1RxGainDb"]    = vac1RxGainDb;
    o["vac1TxGainDb"]    = vac1TxGainDb;
    o["agcMode"]       = agcMode;
    o["autoMuteOnTx"]  = autoMuteOnTx;
    o["alcMaxGainLinear"]     = alcMaxGainLinear;
    o["levelerOn"]            = levelerOn;
    o["levelerMaxGainLinear"] = levelerMaxGainLinear;
    o["levelerDecayMs"]       = levelerDecayMs;
    o["txTimeoutSec"]  = txTimeoutSec;
    o["txTimeoutBypass"] = txTimeoutBypass;
    // Native rack blobs (#49 v3) — omit empties to keep pre-rack profiles
    // byte-clean on disk.
    if (!eq.isEmpty())         o["eq"]         = eq;
    if (!speech.isEmpty())     o["speech"]     = speech;
    if (!combinator.isEmpty()) o["combinator"] = combinator;
    if (!plate.isEmpty())      o["plate"]      = plate;
    return o;
}

Profile Profile::fromJson(const QString &name, const QJsonObject &o) {
    Profile p;
    p.name = name;
    // Tolerant: a missing key keeps the struct default (forward/backward
    // compatible across schemaVersion bumps).
    if (o.contains("schemaVersion")) p.schemaVersion = o["schemaVersion"].toInt(p.schemaVersion);
    if (o.contains("rxBandwidth"))   p.rxBandwidth   = o["rxBandwidth"].toInt(p.rxBandwidth);
    if (o.contains("txBandwidth"))   p.txBandwidth   = o["txBandwidth"].toInt(p.txBandwidth);
    if (o.contains("bwLocked"))      p.bwLocked      = o["bwLocked"].toBool(p.bwLocked);
    if (o.contains("filterLow"))     p.filterLow     = o["filterLow"].toInt(p.filterLow);
    if (o.contains("micSource"))     p.micSource     = o["micSource"].toString(p.micSource);
    if (o.contains("micGainDb"))     p.micGainDb     = o["micGainDb"].toDouble(p.micGainDb);
    if (o.contains("micBoost"))      p.micBoost      = o["micBoost"].toBool(p.micBoost);
    if (o.contains("useTuneDrive"))  p.useTuneDrive  = o["useTuneDrive"].toBool(p.useTuneDrive);
    if (o.contains("tuneDrivePct"))  p.tuneDrivePct  = o["tuneDrivePct"].toInt(p.tuneDrivePct);
    if (o.contains("txDriveLevel"))  p.txDriveLevel  = o["txDriveLevel"].toInt(p.txDriveLevel);
    if (o.contains("tciRxGainDb"))   p.tciRxGainDb   = o["tciRxGainDb"].toDouble(p.tciRxGainDb);
    if (o.contains("tciTxGainDb"))   p.tciTxGainDb   = o["tciTxGainDb"].toDouble(p.tciTxGainDb);
    if (o.contains("vac1Enabled"))     p.vac1Enabled     = o["vac1Enabled"].toBool(p.vac1Enabled);
    if (o.contains("vac1AutoDigital")) p.vac1AutoDigital = o["vac1AutoDigital"].toBool(p.vac1AutoDigital);
    if (o.contains("vac1RxGainDb"))    p.vac1RxGainDb    = o["vac1RxGainDb"].toDouble(p.vac1RxGainDb);
    if (o.contains("vac1TxGainDb"))    p.vac1TxGainDb    = o["vac1TxGainDb"].toDouble(p.vac1TxGainDb);
    if (o.contains("agcMode"))       p.agcMode       = o["agcMode"].toString(p.agcMode);
    if (o.contains("autoMuteOnTx"))  p.autoMuteOnTx  = o["autoMuteOnTx"].toBool(p.autoMuteOnTx);
    if (o.contains("alcMaxGainLinear"))     p.alcMaxGainLinear     = o["alcMaxGainLinear"].toDouble(p.alcMaxGainLinear);
    if (o.contains("levelerOn"))            p.levelerOn            = o["levelerOn"].toBool(p.levelerOn);
    if (o.contains("levelerMaxGainLinear")) p.levelerMaxGainLinear = o["levelerMaxGainLinear"].toDouble(p.levelerMaxGainLinear);
    if (o.contains("levelerDecayMs"))       p.levelerDecayMs       = o["levelerDecayMs"].toInt(p.levelerDecayMs);
    if (o.contains("txTimeoutSec"))  p.txTimeoutSec  = o["txTimeoutSec"].toInt(p.txTimeoutSec);
    if (o.contains("txTimeoutBypass")) p.txTimeoutBypass = o["txTimeoutBypass"].toBool(p.txTimeoutBypass);
    if (o.contains("eq"))         p.eq         = o["eq"].toObject();
    if (o.contains("speech"))     p.speech     = o["speech"].toObject();
    if (o.contains("combinator")) p.combinator = o["combinator"].toObject();
    if (o.contains("plate"))      p.plate      = o["plate"].toObject();
    return p;
}

bool Profile::sameValues(const Profile &b) const {
    auto dEq = [](double x, double y) {
        // operator-facing dB/levels; 1e-6 is far below any audible step.
        return (x - y) < 1e-6 && (y - x) < 1e-6;
    };
    return rxBandwidth == b.rxBandwidth
        && txBandwidth == b.txBandwidth
        && bwLocked == b.bwLocked
        && filterLow == b.filterLow
        && micSource == b.micSource
        && dEq(micGainDb, b.micGainDb)
        && micBoost == b.micBoost
        && useTuneDrive == b.useTuneDrive
        // tuneDrivePct / txDriveLevel deliberately excluded — per-band
        // (BandMemory), not profile fields (see Profile.h).
        && dEq(tciRxGainDb, b.tciRxGainDb)
        && dEq(tciTxGainDb, b.tciTxGainDb)
        && vac1Enabled == b.vac1Enabled
        && vac1AutoDigital == b.vac1AutoDigital
        && dEq(vac1RxGainDb, b.vac1RxGainDb)
        && dEq(vac1TxGainDb, b.vac1TxGainDb)
        && agcMode == b.agcMode
        && autoMuteOnTx == b.autoMuteOnTx
        && dEq(alcMaxGainLinear, b.alcMaxGainLinear)
        && levelerOn == b.levelerOn
        && dEq(levelerMaxGainLinear, b.levelerMaxGainLinear)
        && levelerDecayMs == b.levelerDecayMs
        && txTimeoutSec == b.txTimeoutSec
        && txTimeoutBypass == b.txTimeoutBypass
        // Native rack blobs — QJsonObject has value equality, so any EQ /
        // Speech / Combinator / Plate change flags the profile ● modified
        // (operator-confirmed 2026-06-16: dirty-tracking includes the rack).
        && eq == b.eq
        && speech == b.speech
        && combinator == b.combinator
        && plate == b.plate;
}

}  // namespace lyra::profile
