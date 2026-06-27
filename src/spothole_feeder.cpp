// Lyra — SpotHole REST spot feeder.  See spothole_feeder.h.

#include "spothole_feeder.h"

#include "bandmemory.h"
#include "hl2_stream.h"
#include "prefs.h"
#include "spotstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace lyra::ui {

namespace {
constexpr auto kKeyEnabled  = "spots/spothole/enabled";
constexpr auto kKeyInterval = "spots/spothole/interval_sec";
constexpr auto kKeyMaxAge   = "spots/spothole/max_age_sec";
constexpr auto kKeySource   = "spots/spothole/source";
constexpr auto kKeyBandOnly = "spots/spothole/band_only";
constexpr auto kBaseUrl     = "https://spothole.app/api/v1/spots";
constexpr auto kSourceTag   = "spothole";
}

SpotHoleFeeder::SpotHoleFeeder(SpotStore *store, Prefs *prefs,
                               lyra::ipc::HL2Stream *stream, QObject *parent)
    : QObject(parent), store_(store), prefs_(prefs), stream_(stream) {
    QSettings s;
    enabled_     = s.value(QString::fromLatin1(kKeyEnabled), false).toBool();
    intervalSec_ = qBound(10, s.value(QString::fromLatin1(kKeyInterval), 30).toInt(), 600);
    maxAgeSec_   = qBound(60, s.value(QString::fromLatin1(kKeyMaxAge), 600).toInt(), 7200);
    subSource_   = s.value(QString::fromLatin1(kKeySource),
                           QStringLiteral("Cluster")).toString();
    bandOnly_    = s.value(QString::fromLatin1(kKeyBandOnly), true).toBool();

    nam_   = new QNetworkAccessManager(this);
    timer_ = new QTimer(this);
    timer_->setInterval(intervalSec_ * 1000);
    connect(timer_, &QTimer::timeout, this, &SpotHoleFeeder::fetch);
    if (enabled_) { timer_->start(); fetch(); }   // immediate first poll
}

QString SpotHoleFeeder::currentHamBand() const {
    if (!stream_) return QString();
    const QString name = BandMemory::bandNameFor(int(stream_->rx1FreqHz()));
    // bandNameFor → "40m" / "bc_49m" / "cb_11m" / "".  SpotHole only knows
    // ham bands, so request all bands when on a broadcast/CB segment.
    if (name.isEmpty() || name.contains(QLatin1Char('_'))) return QString();
    return name;
}

void SpotHoleFeeder::fetch() {
    if (!enabled_ || !store_ || fetching_) return;
    fetching_ = true;

    QUrl url{QString::fromLatin1(kBaseUrl)};
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("300"));
    q.addQueryItem(QStringLiteral("max_age"), QString::number(maxAgeSec_));
    if (!subSource_.trimmed().isEmpty())
        q.addQueryItem(QStringLiteral("source"), subSource_.trimmed());
    if (bandOnly_) {
        const QString band = currentHamBand();
        if (!band.isEmpty()) q.addQueryItem(QStringLiteral("band"), band);
    }
    url.setQuery(q);

    QNetworkRequest req{url};
    req.setRawHeader("User-Agent", "Lyra-SDR (Hermes Lite 2 client)");
    req.setTransferTimeout(15000);
    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        fetching_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            emit statusChanged(reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
            emit statusChanged(tr("Bad response"));
            return;
        }
        int n = 0;
        for (const QJsonValue &v : doc.array()) {
            const QJsonObject o = v.toObject();
            // SpotHole v1 raw JSON is snake_case; freq is in Hz (a float).
            if (o.value(QStringLiteral("qrt")).toBool()) continue;   // station went QRT
            const QString call = o.value(QStringLiteral("dx_call")).toString().trimmed();
            if (call.isEmpty()) continue;
            double hz = o.value(QStringLiteral("freq")).toDouble();
            if (hz <= 0.0)
                hz = o.value(QStringLiteral("freq")).toString().toDouble();
            const qint64 freqHz = qint64(std::llround(hz));
            if (freqHz <= 0) continue;
            const QString mode    = o.value(QStringLiteral("mode")).toString();
            const QString comment = o.value(QStringLiteral("comment")).toString();
            const QString spotter = o.value(QStringLiteral("de_call")).toString();
            const QString cont    = o.value(QStringLiteral("dx_continent")).toString();
            // Marker tooltip: comment + who reported it.
            QString text = comment.trimmed();
            if (!spotter.trimmed().isEmpty()) {
                if (!text.isEmpty()) text += QLatin1String("  ");
                text += QStringLiteral("de ") + spotter.trimmed();
            }
            // Marker colour left default (gold); the DXCC/mode visual layer
            // recolours later.  Tagged "spothole" for per-source on/off.
            store_->addSpot(call, mode, freqHz, 0, text,
                            QString::fromLatin1(kSourceTag), cont);
            ++n;
        }
        emit statusChanged(n > 0 ? tr("%1 spot(s)").arg(n) : tr("No spots"));
    });
}

void SpotHoleFeeder::refresh() { if (enabled_) fetch(); }

void SpotHoleFeeder::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyEnabled), on);
    if (on) {
        timer_->start();
        fetch();
    } else {
        timer_->stop();
        // Drop this source's spots so disabling clears the clutter at once.
        if (store_) store_->clearSource(QString::fromLatin1(kSourceTag));
        emit statusChanged(tr("Off"));
    }
}

void SpotHoleFeeder::setIntervalSec(int s) {
    s = qBound(10, s, 600);
    if (intervalSec_ == s) return;
    intervalSec_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyInterval), s);
    timer_->setInterval(s * 1000);
}

void SpotHoleFeeder::setMaxAgeSec(int s) {
    s = qBound(60, s, 7200);
    if (maxAgeSec_ == s) return;
    maxAgeSec_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyMaxAge), s);
}

void SpotHoleFeeder::setSubSource(const QString &s) {
    if (subSource_ == s) return;
    subSource_ = s;
    QSettings().setValue(QString::fromLatin1(kKeySource), s);
    if (enabled_) fetch();
}

void SpotHoleFeeder::setCurrentBandOnly(bool on) {
    if (bandOnly_ == on) return;
    bandOnly_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyBandOnly), on);
    if (enabled_) fetch();
}

} // namespace lyra::ui
