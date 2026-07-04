// Lyra — weather-alert service.  See wxservice.h.  Aggregation + tier
// classification ported from old Lyra's wx/aggregator.py; per-source
// endpoints/parsing from wx/sources/*.py (Blitzortung / NWS / Ambient /
// Ecowitt).  Async QNetworkAccessManager, 30 s poll, no worker thread.

#include "wxservice.h"

#include "prefs.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <functional>
#include <memory>

namespace lyra::wx {

namespace {
constexpr int    kPollMs        = 30000;            // 30 s
constexpr qint64 kHysteresisMs  = 15LL * 60 * 1000; // 15 min per condition
constexpr double kEarthKm       = 6371.0;
constexpr double kPi            = 3.14159265358979323846;

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180.0;
    const double dlat = (lat2 - lat1) * r, dlon = (lon2 - lon1) * r;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
        std::cos(lat1 * r) * std::cos(lat2 * r) *
        std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * kEarthKm * std::asin(std::sqrt(std::min(1.0, a)));
}
double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180.0;
    const double dlon = (lon2 - lon1) * r;
    const double y = std::sin(dlon) * std::cos(lat2 * r);
    const double x = std::cos(lat1 * r) * std::sin(lat2 * r) -
        std::sin(lat1 * r) * std::cos(lat2 * r) * std::cos(dlon);
    return std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
}
double msToMph(double ms) { return ms * 2.23694; }
} // namespace

// Per-cycle accumulator: closest lightning (min), strongest wind (max),
// severe/wind headlines, and success/fail tallies.
struct WxService::PollCtx {
    int     gen     = 0;
    int     pending = 0;
    double  closestKm = -1.0;
    double  bearing   = 0.0;
    double  windS = 0.0, windG = 0.0;
    int     windDir = -1;
    bool    nwsExtreme = false;
    QString windHeadline, severeHeadline;
    int     tried = 0, failed = 0;
};

WxService::WxService(lyra::ui::Prefs *prefs, QObject *parent)
    : QObject(parent), prefs_(prefs),
      nam_(new QNetworkAccessManager(this)),
      timer_(new QTimer(this)) {
    timer_->setInterval(kPollMs);
    connect(timer_, &QTimer::timeout, this, &WxService::poll);
    reloadConfig();
}

void WxService::reloadConfig() {
    QSettings s;
    WxConfig c;
    c.enabled    = s.value(QStringLiteral("wx/enabled"), false).toBool();
    c.disclaimer = s.value(QStringLiteral("wx/disclaimer_accepted"), false).toBool();
    c.srcBlitz   = s.value(QStringLiteral("wx/src_blitzortung"), true).toBool();
    c.srcNws     = s.value(QStringLiteral("wx/src_nws"), true).toBool();
    c.srcNwsMetar = s.value(QStringLiteral("wx/src_nws_metar"), true).toBool();
    c.srcAmbient = s.value(QStringLiteral("wx/src_ambient"), false).toBool();
    c.srcEcowitt = s.value(QStringLiteral("wx/src_ecowitt"), false).toBool();
    c.lightningRangeKm = s.value(QStringLiteral("wx/lightning_range_km"), 80.0).toDouble();
    c.windSustainedMph = s.value(QStringLiteral("wx/wind_sustained_mph"), 30.0).toDouble();
    c.windGustMph      = s.value(QStringLiteral("wx/wind_gust_mph"), 40.0).toDouble();
    c.ambientApiKey = s.value(QStringLiteral("wx/ambient_api_key")).toString();
    c.ambientAppKey = s.value(QStringLiteral("wx/ambient_app_key")).toString();
    c.ecowittAppKey = s.value(QStringLiteral("wx/ecowitt_app_key")).toString();
    c.ecowittApiKey = s.value(QStringLiteral("wx/ecowitt_api_key")).toString();
    c.ecowittMac    = s.value(QStringLiteral("wx/ecowitt_mac")).toString();
    c.nwsMetarStation = s.value(QStringLiteral("wx/nws_metar_station")).toString();
    c.distanceUnit  = s.value(QStringLiteral("wx/distance_unit"), QStringLiteral("mi")).toString();
    c.windUnit      = s.value(QStringLiteral("wx/wind_unit"), QStringLiteral("mph")).toString();
    c.desktopEnabled = s.value(QStringLiteral("wx/desktop_enabled"), true).toBool();
    c.audioEnabled   = s.value(QStringLiteral("wx/audio_enabled"), true).toBool();
    c.nwsWindToast   = s.value(QStringLiteral("wx/nws_wind_toast"), false).toBool();
    if (prefs_) c.haveLoc = prefs_->operatorLocation(&c.lat, &c.lon);

    const bool wasEnabled = cfg_.enabled;
    cfg_ = c;
    if (cfg_.enabled) {
        if (!timer_->isActive()) timer_->start();
        poll();                          // immediate first read
    } else {
        timer_->stop();
        if (last_.lightning != Lightning::None || last_.wind != Wind::None ||
            last_.severe != Severe::None || !last_.error.isEmpty()) {
            last_ = WxSnapshot();        // clear stale badges
            emit snapshotChanged(last_);
        }
    }
    if (wasEnabled != cfg_.enabled) emit enabledChanged(cfg_.enabled);
}

void WxService::checkNow() { if (cfg_.enabled) poll(); }

void WxService::poll() {
    if (!cfg_.enabled) return;
    auto ctx = std::make_shared<PollCtx>();
    ctx->gen = ++gen_;
    WxService *self = this;

    auto fire = [self, ctx](const QString &url, const QByteArray &ua,
                            const QByteArray &ref,
                            std::function<void(const QByteArray &)> parse) {
        ctx->pending++;
        QNetworkRequest r{QUrl(url)};
        if (!ua.isEmpty())  r.setRawHeader("User-Agent", ua);
        if (!ref.isEmpty()) r.setRawHeader("Referer", ref);
        r.setTransferTimeout(12000);
        QNetworkReply *reply = self->nam_->get(r);
        QObject::connect(reply, &QNetworkReply::finished, self,
                         [self, ctx, reply, parse]() {
            reply->deleteLater();
            const bool stale = (ctx->gen != self->gen_);
            if (!stale) {
                ctx->tried++;
                if (reply->error() != QNetworkReply::NoError) ctx->failed++;
                else parse(reply->readAll());
            }
            if (--ctx->pending == 0 && !stale) self->finalize(ctx.get());
        });
    };

    const QByteArray uaNws("Lyra-SDR (Hermes Lite 2 client)");
    const QByteArray uaBlitz("Mozilla/5.0 (Windows NT 10.0; Win64; x64) Lyra-SDR");
    const QByteArray refBlitz("https://map.blitzortung.org/");

    if (cfg_.srcBlitz && cfg_.haveLoc) {
        for (int region : {7, 12, 13}) {
            const QString url = QStringLiteral(
                "https://map.blitzortung.org/GEOjson/getjson.php?f=s&n=%1")
                .arg(region, 2, 10, QChar('0'));
            fire(url, uaBlitz, refBlitz,
                 [self, ctx](const QByteArray &b) { self->parseBlitz(b, ctx.get()); });
        }
    }
    if (cfg_.srcNws && cfg_.haveLoc) {
        const QString url = QStringLiteral(
            "https://api.weather.gov/alerts/active?point=%1,%2")
            .arg(cfg_.lat, 0, 'f', 4).arg(cfg_.lon, 0, 'f', 4);
        fire(url, uaNws, QByteArray(),
             [self, ctx](const QByteArray &b) { self->parseNwsAlerts(b, ctx.get()); });
    }
    if (cfg_.srcNwsMetar && cfg_.nwsMetarStation.size() >= 3) {
        const QString url = QStringLiteral(
            "https://api.weather.gov/stations/%1/observations/latest")
            .arg(cfg_.nwsMetarStation.toUpper());
        fire(url, uaNws, QByteArray(),
             [self, ctx](const QByteArray &b) { self->parseMetar(b, ctx.get()); });
    }
    if (cfg_.srcAmbient && !cfg_.ambientApiKey.isEmpty() &&
        !cfg_.ambientAppKey.isEmpty()) {
        const QString url = QStringLiteral(
            "https://rt.ambientweather.net/v1/devices?apiKey=%1&applicationKey=%2")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(cfg_.ambientApiKey)),
                 QString::fromUtf8(QUrl::toPercentEncoding(cfg_.ambientAppKey)));
        fire(url, QByteArray(), QByteArray(),
             [self, ctx](const QByteArray &b) { self->parseAmbient(b, ctx.get()); });
    }
    if (cfg_.srcEcowitt && !cfg_.ecowittAppKey.isEmpty() &&
        !cfg_.ecowittApiKey.isEmpty() && !cfg_.ecowittMac.isEmpty()) {
        QUrl u(QStringLiteral("https://api.ecowitt.net/api/v3/device/real_time"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("application_key"), cfg_.ecowittAppKey);
        q.addQueryItem(QStringLiteral("api_key"), cfg_.ecowittApiKey);
        q.addQueryItem(QStringLiteral("mac"), cfg_.ecowittMac.toUpper());
        q.addQueryItem(QStringLiteral("call_back"), QStringLiteral("all"));
        q.addQueryItem(QStringLiteral("wind_unitid"), QStringLiteral("9"));  // mph
        u.setQuery(q);
        fire(u.toString(), QByteArray(), QByteArray(),
             [self, ctx](const QByteArray &b) { self->parseEcowitt(b, ctx.get()); });
    }

    if (ctx->pending == 0) finalize(ctx.get());   // nothing to fetch
}

// ---- per-source parsers (fold into the accumulator) ----

void WxService::parseBlitz(const QByteArray &body, PollCtx *ctx) const {
    // JSON array of [lon, lat, ...] (old Lyra blitzortung.py).
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isArray()) return;
    for (const QJsonValue &v : doc.array()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 2) continue;
        const double sLon = a.at(0).toDouble();
        const double sLat = a.at(1).toDouble();
        const double d = haversineKm(cfg_.lat, cfg_.lon, sLat, sLon);
        if (d <= cfg_.lightningRangeKm &&
            (ctx->closestKm < 0 || d < ctx->closestKm)) {
            ctx->closestKm = d;
            ctx->bearing = bearingDeg(cfg_.lat, cfg_.lon, sLat, sLon);
        }
    }
}

void WxService::parseNwsAlerts(const QByteArray &body, PollCtx *ctx) const {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return;
    static const char *kWind[] = {"high wind warning", "high wind watch",
                                  "wind advisory", "extreme wind warning"};
    for (const QJsonValue &fv : doc.object().value(QStringLiteral("features")).toArray()) {
        const QJsonObject props = fv.toObject().value(QStringLiteral("properties")).toObject();
        const QString event = props.value(QStringLiteral("event")).toString();
        const QString lc = event.toLower();
        if (ctx->severeHeadline.isEmpty() &&
            (lc.contains(QStringLiteral("thunderstorm")) ||
             lc.contains(QStringLiteral("lightning")))) {
            ctx->severeHeadline = event;
        }
        if (ctx->windHeadline.isEmpty()) {
            for (const char *w : kWind) {
                if (lc.contains(QLatin1String(w))) {
                    ctx->windHeadline = event;
                    if (lc.contains(QStringLiteral("high wind warning")) ||
                        lc.contains(QStringLiteral("extreme wind")))
                        ctx->nwsExtreme = true;
                    break;
                }
            }
        }
    }
}

void WxService::parseMetar(const QByteArray &body, PollCtx *ctx) const {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return;
    const QJsonObject props = doc.object().value(QStringLiteral("properties")).toObject();
    auto val = [&props](const char *k) -> double {
        const QJsonValue v = props.value(QLatin1String(k)).toObject().value(QStringLiteral("value"));
        return v.isDouble() ? v.toDouble() : -1.0;   // null/absent -> -1
    };
    const double sMs = val("windSpeed");
    const double gMs = val("windGust");
    const double dir = val("windDirection");
    if (sMs >= 0) ctx->windS = std::max(ctx->windS, msToMph(sMs));
    if (gMs >= 0) ctx->windG = std::max(ctx->windG, msToMph(gMs));
    if (dir >= 0 && ctx->windDir < 0) ctx->windDir = static_cast<int>(dir);
}

void WxService::parseAmbient(const QByteArray &body, PollCtx *ctx) const {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isArray() || doc.array().isEmpty()) return;
    const QJsonObject last =
        doc.array().at(0).toObject().value(QStringLiteral("lastData")).toObject();
    // Lightning (WH31L add-on): distance in miles + strikes this hour.
    const QJsonValue distMi = last.value(QStringLiteral("lightning_distance"));
    const double hour = last.value(QStringLiteral("lightning_hour")).toDouble(0);
    if (distMi.isDouble() && hour > 0) {
        const double d = distMi.toDouble() * 1.60934;
        if (d <= cfg_.lightningRangeKm && (ctx->closestKm < 0 || d < ctx->closestKm)) {
            ctx->closestKm = d;   // station-relative; no bearing available
        }
    }
    if (last.contains(QStringLiteral("windspeedmph")))
        ctx->windS = std::max(ctx->windS, last.value(QStringLiteral("windspeedmph")).toDouble());
    if (last.contains(QStringLiteral("windgustmph")))
        ctx->windG = std::max(ctx->windG, last.value(QStringLiteral("windgustmph")).toDouble());
    if (ctx->windDir < 0 && last.contains(QStringLiteral("winddir")))
        ctx->windDir = static_cast<int>(last.value(QStringLiteral("winddir")).toDouble());
}

void WxService::parseEcowitt(const QByteArray &body, PollCtx *ctx) const {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("code")).toInt(-1) != 0) return;
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    auto leaf = [&data](const char *grp, const char *key) -> double {
        const QJsonObject g = data.value(QLatin1String(grp)).toObject();
        const QJsonValue v = g.value(QLatin1String(key)).toObject().value(QStringLiteral("value"));
        return v.isString() ? v.toString().toDouble()
                            : (v.isDouble() ? v.toDouble() : std::nan(""));
    };
    const double distKm = leaf("lightning", "distance");
    const double hour   = leaf("lightning", "count_hour");
    if (!std::isnan(distKm) && hour > 0 && distKm <= cfg_.lightningRangeKm &&
        (ctx->closestKm < 0 || distKm < ctx->closestKm)) {
        ctx->closestKm = distKm;
    }
    const double ws = leaf("wind", "wind_speed");
    const double wg = leaf("wind", "wind_gust");
    const double wd = leaf("wind", "wind_direction");
    if (!std::isnan(ws)) ctx->windS = std::max(ctx->windS, ws);
    if (!std::isnan(wg)) ctx->windG = std::max(ctx->windG, wg);
    if (!std::isnan(wd) && ctx->windDir < 0) ctx->windDir = static_cast<int>(wd);
}

void WxService::finalize(PollCtx *ctx) {
    WxSnapshot snap;

    // Lightning tier by closest strike (old Lyra _classify_lightning).
    if (ctx->closestKm >= 0) {
        snap.closestKm = ctx->closestKm;
        snap.bearingDeg = ctx->bearing;
        if (ctx->closestKm <= cfg_.lightningCloseKm)      snap.lightning = Lightning::Close;
        else if (ctx->closestKm <= cfg_.lightningMidKm)   snap.lightning = Lightning::Mid;
        else if (ctx->closestKm <= cfg_.lightningRangeKm) snap.lightning = Lightning::Far;
    }
    // Wind tier (old Lyra _classify_wind).
    snap.windSustainedMph = ctx->windS;
    snap.windGustMph = ctx->windG;
    snap.windDirDeg = ctx->windDir;
    snap.windHeadline = ctx->windHeadline;
    snap.windNwsWarning = ctx->nwsExtreme;
    const double s = ctx->windS, g = ctx->windG;
    if (ctx->nwsExtreme ||
        s >= cfg_.windSustainedMph + 15 || g >= cfg_.windGustMph + 15)
        snap.wind = Wind::Extreme;
    else if (s >= cfg_.windSustainedMph || g >= cfg_.windGustMph)
        snap.wind = Wind::High;
    else if (s >= cfg_.windSustainedMph - 10 || g >= cfg_.windGustMph - 10)
        snap.wind = (s > 0 || g > 0) ? Wind::Elevated : Wind::None;
    // Severe.
    if (!ctx->severeHeadline.isEmpty()) {
        snap.severe = Severe::Active;
        snap.severeHeadline = ctx->severeHeadline;
    }
    // All enabled sources failed → surface an error (badges stay clear).
    if (ctx->tried > 0 && ctx->failed == ctx->tried)
        snap.error = tr("all weather sources unreachable");
    else if (cfg_.enabled && !cfg_.haveLoc && ctx->tried == 0)
        snap.error = tr("operator location not set");

    last_ = snap;
    emit snapshotChanged(snap);
    maybeToast(snap);
}

void WxService::maybeToast(const WxSnapshot &snap) {
    if (testActive_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // ── DIAGNOSTIC (always logged) — proves, from the operator's own
    // hardware, exactly what the C++ weather service decided this poll:
    // the wind readings, the thresholds applied, the tier, and whether a
    // toast WILL fire.  If a desktop notice pops while these lines show
    // no FIRE=1, that notice did not come from Lyra-cpp.
    {
        const char *wt = snap.wind == Wind::Extreme  ? "EXTREME"
                       : snap.wind == Wind::High      ? "high"
                       : snap.wind == Wind::Elevated  ? "elevated" : "none";
        const bool localExtremeDiag =
            snap.windSustainedMph >= cfg_.windSustainedMph + 15 ||
            snap.windGustMph      >= cfg_.windGustMph + 15;
        const bool windFire =
            snap.wind == Wind::Extreme &&
            (localExtremeDiag || (snap.windNwsWarning && cfg_.nwsWindToast));
        qDebug().noquote()   // routine per-poll diagnostic, not a warning
            << QStringLiteral("[wx-diag] wind S=%1 G=%2 mph  thr=%3/%4  +15=%5/%6  "
                              "tier=%7 nwsWarn=%8 nwsToggle=%9  WIND_FIRE=%10  | "
                              "lightning=%11 severe=%12")
                   .arg(snap.windSustainedMph, 0, 'f', 1)
                   .arg(snap.windGustMph, 0, 'f', 1)
                   .arg(cfg_.windSustainedMph, 0, 'f', 0)
                   .arg(cfg_.windGustMph, 0, 'f', 0)
                   .arg(cfg_.windSustainedMph + 15, 0, 'f', 0)
                   .arg(cfg_.windGustMph + 15, 0, 'f', 0)
                   .arg(QString::fromLatin1(wt))
                   .arg(snap.windNwsWarning ? 1 : 0)
                   .arg(cfg_.nwsWindToast ? 1 : 0)
                   .arg(windFire ? 1 : 0)
                   .arg(snap.lightning == Lightning::Close ? "CLOSE" : "-")
                   .arg(snap.severe == Severe::Active ? "ACTIVE" : "-");
    }
    auto cross = [this](const QString &t, const QString &b) {
        if (cfg_.desktopEnabled) emit toast(t, b);
        if (cfg_.audioEnabled)   emit chime();
    };
    // Lightning CLOSE (≤ close tier).
    if (snap.lightning == Lightning::Close) {
        if (now - lastLightToastMs_ >= kHysteresisMs) {
            lastLightToastMs_ = now;
            cross(tr("⚡ Lightning detected nearby"),
                  tr("Closest strike ~%1 — consider disconnecting antennas.")
                      .arg(cfg_.distanceUnit == QStringLiteral("km")
                           ? tr("%1 km").arg(qRound(snap.closestKm))
                           : tr("%1 mi").arg(qRound(snap.closestKm / 1.60934))));
        }
    } else {
        lastLightToastMs_ = 0;   // cleared → re-arm
    }
    // Wind EXTREME.  Local-threshold Extreme (sustained/gust crossed the
    // operator's set values + 15) always toasts.  An Extreme tier forced
    // solely by an NWS High/Extreme Wind Warning headline — independent of
    // the local readings — toasts only when the operator opts in
    // (wx/nws_wind_toast); the header badge still reflects it regardless.
    const bool localExtreme =
        snap.windSustainedMph >= cfg_.windSustainedMph + 15 ||
        snap.windGustMph      >= cfg_.windGustMph + 15;
    const bool wantWindToast =
        snap.wind == Wind::Extreme &&
        (localExtreme || (snap.windNwsWarning && cfg_.nwsWindToast));
    if (wantWindToast) {
        if (now - lastWindToastMs_ >= kHysteresisMs) {
            lastWindToastMs_ = now;
            cross(tr("💨 Extreme wind"),
                  snap.windHeadline.isEmpty()
                      ? tr("Gusts to %1 mph").arg(qRound(snap.windGustMph))
                      : snap.windHeadline);
        }
    } else {
        lastWindToastMs_ = 0;
    }
    // Severe storm warning.
    if (snap.severe == Severe::Active) {
        if (now - lastSevereToastMs_ >= kHysteresisMs) {
            lastSevereToastMs_ = now;
            cross(tr("⚠ NWS storm warning"), snap.severeHeadline);
        }
    } else {
        lastSevereToastMs_ = 0;
    }
}

void WxService::fireTestSnapshot() {
    // Inject a fake "all alerts active" snapshot for 6 s so the operator
    // can see the header badges + fire one test toast.
    testActive_ = true;
    WxSnapshot snap;
    snap.lightning = Lightning::Close;  snap.closestKm = 12.0; snap.bearingDeg = 200;
    snap.wind = Wind::Extreme;          snap.windSustainedMph = 45; snap.windGustMph = 60;
    snap.windDirDeg = 270;
    snap.severe = Severe::Active;       snap.severeHeadline = tr("TEST — Severe Thunderstorm Warning");
    last_ = snap;
    emit snapshotChanged(snap);
    emit toast(tr("Lyra weather — test"),
               tr("This is a test alert. Lightning / wind / severe badges "
                  "should be lit for a few seconds."));
    QTimer::singleShot(6000, this, [this]() {
        testActive_ = false;
        if (cfg_.enabled) poll();          // back to live
        else { last_ = WxSnapshot(); emit snapshotChanged(last_); }
    });
}

} // namespace lyra::wx
