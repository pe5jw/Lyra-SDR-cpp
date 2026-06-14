// Lyra — Profile JSON (de)serialize.  See Profile.h.

#include "profile/Profile.h"

namespace lyra::profile {

QJsonObject Profile::toJson() const {
    QJsonObject o;
    o["schemaVersion"] = schemaVersion;
    o["mode"]          = mode;
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
    o["agcMode"]       = agcMode;
    o["autoMuteOnTx"]  = autoMuteOnTx;
    o["txTimeoutSec"]  = txTimeoutSec;
    o["txTimeoutBypass"] = txTimeoutBypass;
    return o;
}

Profile Profile::fromJson(const QString &name, const QJsonObject &o) {
    Profile p;
    p.name = name;
    // Tolerant: a missing key keeps the struct default (forward/backward
    // compatible across schemaVersion bumps).
    if (o.contains("schemaVersion")) p.schemaVersion = o["schemaVersion"].toInt(p.schemaVersion);
    if (o.contains("mode"))          p.mode          = o["mode"].toString(p.mode);
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
    if (o.contains("agcMode"))       p.agcMode       = o["agcMode"].toString(p.agcMode);
    if (o.contains("autoMuteOnTx"))  p.autoMuteOnTx  = o["autoMuteOnTx"].toBool(p.autoMuteOnTx);
    if (o.contains("txTimeoutSec"))  p.txTimeoutSec  = o["txTimeoutSec"].toInt(p.txTimeoutSec);
    if (o.contains("txTimeoutBypass")) p.txTimeoutBypass = o["txTimeoutBypass"].toBool(p.txTimeoutBypass);
    return p;
}

bool Profile::sameValues(const Profile &b) const {
    auto dEq = [](double x, double y) {
        // operator-facing dB/levels; 1e-6 is far below any audible step.
        return (x - y) < 1e-6 && (y - x) < 1e-6;
    };
    return mode == b.mode
        && rxBandwidth == b.rxBandwidth
        && txBandwidth == b.txBandwidth
        && bwLocked == b.bwLocked
        && filterLow == b.filterLow
        && micSource == b.micSource
        && dEq(micGainDb, b.micGainDb)
        && micBoost == b.micBoost
        && useTuneDrive == b.useTuneDrive
        && tuneDrivePct == b.tuneDrivePct
        && txDriveLevel == b.txDriveLevel
        && dEq(tciRxGainDb, b.tciRxGainDb)
        && dEq(tciTxGainDb, b.tciTxGainDb)
        && agcMode == b.agcMode
        && autoMuteOnTx == b.autoMuteOnTx
        && txTimeoutSec == b.txTimeoutSec
        && txTimeoutBypass == b.txTimeoutBypass;
}

}  // namespace lyra::profile
