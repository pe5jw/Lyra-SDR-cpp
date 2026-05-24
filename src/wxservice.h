// Lyra — weather-alert service (ported from old Lyra's wx/ package).
//
// Aggregates lightning + wind + severe-storm data from up to four
// operator-toggleable sources (Blitzortung, NWS/weather.gov, Ambient
// Weather, Ecowitt), classifies them into alert tiers, and emits a
// WxSnapshot on change.  Native C++/Qt: async QNetworkAccessManager
// GETs on a 30 s poll, no worker thread, no Python.  Operator location
// (for the location-based sources) comes from Prefs (grid/lat-lon).

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QTimer;

namespace lyra::ui { class Prefs; }

namespace lyra::wx {

enum class Lightning { None, Far, Mid, Close };
enum class Wind      { None, Elevated, High, Extreme };
enum class Severe    { None, Active };

// Operator-tunable config, rebuilt from QSettings (wx/*) + Prefs location.
struct WxConfig {
    bool   enabled        = false;
    bool   disclaimer     = false;
    // sources
    bool   srcBlitz       = true;
    bool   srcNws         = true;
    bool   srcNwsMetar    = true;
    bool   srcAmbient     = false;
    bool   srcEcowitt     = false;
    // thresholds (km / mph)
    double lightningRangeKm = 80.0;   // far/outer
    double lightningMidKm   = 40.0;
    double lightningCloseKm = 16.0;
    double windSustainedMph = 30.0;
    double windGustMph      = 40.0;
    // credentials
    QString ambientApiKey, ambientAppKey;
    QString ecowittAppKey, ecowittApiKey, ecowittMac;
    QString nwsMetarStation;
    // display units
    QString distanceUnit = QStringLiteral("mi");   // "mi" / "km"
    QString windUnit     = QStringLiteral("mph");  // "mph" / "kt" / "km/h"
    // notifications
    bool   desktopEnabled = true;
    bool   audioEnabled   = true;
    // operator location (resolved from Prefs)
    bool   haveLoc = false;
    double lat = 0.0, lon = 0.0;
};

struct WxSnapshot {
    Lightning lightning   = Lightning::None;
    double    closestKm   = -1.0;   // <0 = no strike in range
    double    bearingDeg  = 0.0;
    Wind      wind        = Wind::None;
    double    windSustainedMph = 0.0;
    double    windGustMph      = 0.0;
    int       windDirDeg  = -1;     // <0 = unknown
    Severe    severe      = Severe::None;
    QString   severeHeadline;
    QString   windHeadline;
    QString   error;                // non-empty if all enabled sources failed
};

class WxService : public QObject {
    Q_OBJECT
public:
    explicit WxService(lyra::ui::Prefs *prefs, QObject *parent = nullptr);

    // Rebuild config from QSettings (wx/*) + Prefs location, and (re)arm
    // the 30 s poll if enabled.  Call after any Settings change or a
    // location change.
    void reloadConfig();

    bool enabled() const { return cfg_.enabled; }
    const WxSnapshot &lastSnapshot() const { return last_; }
    QString distanceUnit() const { return cfg_.distanceUnit; }
    QString windUnit() const { return cfg_.windUnit; }

    Q_INVOKABLE void checkNow();          // force an immediate poll
    void fireTestSnapshot();              // 6 s fake "all active" (test button)

signals:
    void snapshotChanged(const lyra::wx::WxSnapshot &snap);
    void enabledChanged(bool on);
    // Tier-crossing notification (hysteresis-gated) for the OS toast.
    void toast(const QString &title, const QString &body);
    // Audible-cue request (emitted alongside toast when audio is enabled).
    void chime();

private:
    struct PollCtx;                       // per-cycle async accumulator
    void poll();
    void finalize(PollCtx *ctx);
    void maybeToast(const WxSnapshot &snap);
    // Per-source response parsers — each folds its result into the
    // cycle accumulator (closest lightning, max wind, severe headline).
    void parseBlitz(const QByteArray &body, PollCtx *ctx) const;
    void parseNwsAlerts(const QByteArray &body, PollCtx *ctx) const;
    void parseMetar(const QByteArray &body, PollCtx *ctx) const;
    void parseAmbient(const QByteArray &body, PollCtx *ctx) const;
    void parseEcowitt(const QByteArray &body, PollCtx *ctx) const;

    lyra::ui::Prefs        *prefs_  = nullptr;
    QNetworkAccessManager  *nam_    = nullptr;
    QTimer                 *timer_  = nullptr;
    WxConfig                cfg_;
    WxSnapshot              last_;
    int                     gen_    = 0;   // poll generation (discards stale replies)
    // hysteresis: monotonic ms of last toast per condition (0 = never/cleared)
    qint64                  lastLightToastMs_  = 0;
    qint64                  lastWindToastMs_   = 0;
    qint64                  lastSevereToastMs_ = 0;
    bool                    testActive_ = false;
};

} // namespace lyra::wx
