// Lyra — EiBi shortwave-broadcaster overlay.  See eibistore.h.

#include "eibistore.h"

#include "bands.h"
#include "prefs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSslError>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>

namespace lyra::ui {

namespace {
constexpr int  kMaxEntries = 160;   // declutter cap for one span
constexpr auto kKeyEnabled  = "swdb/overlay_master_enabled";
constexpr auto kKeyHideOff   = "swdb/hide_off_air";
constexpr auto kKeyForceAll  = "swdb/overlay_force_all_bands";
constexpr auto kKeyMinPower   = "swdb/min_power";
constexpr auto kKeyFilePath  = "swdb/file_path";

// "2300-0100" → (1380, 60).  "2400" clamps to 1440.  Returns false on junk.
bool parseTim(const QString &s, int *start, int *stop) {
    const int dash = s.indexOf(QLatin1Char('-'));
    if (dash < 0) return false;
    bool a = false, b = false;
    const int hs = s.left(dash).toInt(&a);
    const int he = s.mid(dash + 1).toInt(&b);
    if (!a || !b) return false;
    auto toMin = [](int hhmm) {
        if (hhmm == 2400) return 1440;
        return (hhmm / 100) * 60 + (hhmm % 100);
    };
    *start = toMin(hs);
    *stop  = toMin(he);
    return true;
}

// "246" → bits for Tue/Thu/Sat.  Empty → 0 (every day).
quint8 parseDays(const QString &s) {
    quint8 m = 0;
    for (const QChar c : s) {
        const int d = c.digitValue();
        if (d >= 1 && d <= 7) m |= quint8(1u << (d - 1));
    }
    return m;
}
} // namespace

EibiStore::EibiStore(Prefs *prefs, Bands *bands, QObject *parent)
    : QObject(parent), prefs_(prefs), bands_(bands) {
    QSettings s;
    enabled_    = s.value(QString::fromLatin1(kKeyEnabled), false).toBool();
    hideOffAir_ = s.value(QString::fromLatin1(kKeyHideOff), true).toBool();
    forceAll_   = s.value(QString::fromLatin1(kKeyForceAll), false).toBool();
    minPower_   = s.value(QString::fromLatin1(kKeyMinPower), 1).toInt();
    reload();   // load whatever is on disk (no-op if missing)
}

QString EibiStore::currentSeasonTag() {
    const QDate d = QDate::currentDate();
    const int m = d.month();
    int yy;
    QChar season;
    if (m >= 4 && m <= 10) { season = QLatin1Char('a'); yy = d.year() % 100; }
    else {
        season = QLatin1Char('b');
        yy = (m <= 3 ? d.year() - 1 : d.year()) % 100;   // B season starts in Oct
    }
    return QString(season) + QString::asprintf("%02d", yy);
}

QString EibiStore::seasonFilePath() const {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/swdb");
    return dir + QStringLiteral("/sked-") + currentSeasonTag() + QStringLiteral(".csv");
}

QString EibiStore::filePath() const {
    QSettings s;
    const QString custom = s.value(QString::fromLatin1(kKeyFilePath)).toString();
    return custom.isEmpty() ? seasonFilePath() : custom;
}

bool EibiStore::parseFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    QVector<Entry> out;
    out.reserve(40000);
    bool first = true;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.isEmpty()) continue;
        const QStringList c = line.split(QLatin1Char(';'));
        if (c.size() < 7) continue;
        if (first) { first = false;
            if (c[0].compare(QStringLiteral("KHZ"), Qt::CaseInsensitive) == 0)
                continue;                       // header row
        }
        bool okF = false;
        const int khz = int(qRound(c[0].trimmed().toDouble(&okF)));
        if (!okF || khz < 100 || khz > 35000) continue;
        Entry e;
        e.freqKhz = khz;
        if (!parseTim(c[1].trimmed(), &e.tStart, &e.tStop)) { e.tStart = e.tStop = 0; }
        e.days    = parseDays(c[2].trimmed());
        e.station = c[4].trimmed();
        if (e.station.isEmpty()) continue;
        e.lang    = c[5].trimmed();
        e.target  = c[6].trimmed();
        if (c.size() > 8) {
            bool okP = false;
            const int p = c[8].trimmed().toInt(&okP);
            e.power = quint8(okP ? std::clamp(p, 0, 3) : 0);
        }
        out.append(e);
    }
    std::sort(out.begin(), out.end(),
              [](const Entry &a, const Entry &b) { return a.freqKhz < b.freqKhz; });
    entries_   = std::move(out);
    loaded_    = !entries_.isEmpty();
    loadedFrom_= path;
    return loaded_;
}

bool EibiStore::reload() {
    const bool ok = parseFile(filePath());
    if (!ok) { loaded_ = false; }
    emit changed();
    return ok;
}

QString EibiStore::statusText() const {
    if (loaded_)
        return tr("Loaded %1 entries from %2")
            .arg(entries_.size())
            .arg(QDir::toNativeSeparators(loadedFrom_));
    return tr("No EiBi database loaded — click “Update database now”.");
}

bool EibiStore::isOnAir(const Entry &e) const {
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const int dow = utc.date().dayOfWeek();          // 1=Mon..7=Sun
    if (e.days != 0 && !(e.days & quint8(1u << (dow - 1)))) return false;
    const int nowMin = utc.time().hour() * 60 + utc.time().minute();
    const int s = e.tStart, en = e.tStop;
    if (s == en) return false;                       // zero-length = off
    if (s < en)  return s <= nowMin && nowMin < en;  // same-day window
    return nowMin >= s || nowMin < en;               // wraps past midnight
}

bool EibiStore::gatedOff(double centerHz) const {
    if (forceAll_) return false;
    return bands_ && bands_->indexForFreq(centerHz) >= 0;   // inside a ham band
}

QVariantList EibiStore::entriesInSpan(double centerHz, double spanHz) const {
    QVariantList out;
    if (!enabled_ || !loaded_ || spanHz <= 0 || gatedOff(centerHz)) return out;
    const int loKhz = int((centerHz - spanHz / 2.0) / 1000.0);
    const int hiKhz = int((centerHz + spanHz / 2.0) / 1000.0) + 1;
    // Binary-search the sorted vector to the visible kHz window.
    auto lo = std::lower_bound(entries_.cbegin(), entries_.cend(), loKhz,
        [](const Entry &e, int k) { return e.freqKhz < k; });
    auto hi = std::upper_bound(entries_.cbegin(), entries_.cend(), hiKhz,
        [](int k, const Entry &e) { return k < e.freqKhz; });
    for (auto it = lo; it != hi && out.size() < kMaxEntries; ++it) {
        if (it->power < minPower_) continue;
        const bool on = isOnAir(*it);
        if (hideOffAir_ && !on) continue;
        QVariantMap m;
        m[QStringLiteral("freqHz")]   = double(qint64(it->freqKhz) * 1000);
        m[QStringLiteral("station")]  = it->station;
        m[QStringLiteral("language")] = it->lang;
        m[QStringLiteral("target")]   = it->target;
        m[QStringLiteral("onAir")]    = on;
        out.append(m);
    }
    return out;
}

void EibiStore::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyEnabled), on);
    emit settingsChanged();
}
void EibiStore::setHideOffAir(bool on) {
    if (hideOffAir_ == on) return;
    hideOffAir_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyHideOff), on);
    emit settingsChanged();
}
void EibiStore::setForceAllBands(bool on) {
    if (forceAll_ == on) return;
    forceAll_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyForceAll), on);
    emit settingsChanged();
}
void EibiStore::setMinPower(int p) {
    p = std::clamp(p, 0, 3);
    if (minPower_ == p) return;
    minPower_ = p;
    QSettings().setValue(QString::fromLatin1(kKeyMinPower), p);
    emit settingsChanged();
}

void EibiStore::update() {
    const QString tag = currentSeasonTag();
    fetch(QUrl(QStringLiteral("https://eibispace.de/dx/sked-") + tag
               + QStringLiteral(".csv")),
          /*allowHttpFallback=*/true);
}

void EibiStore::fetch(const QUrl &url, bool allowHttpFallback) {
    if (!net_) net_ = new QNetworkAccessManager(this);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Lyra-SDR EiBi updater"));
    QNetworkReply *reply = net_->get(req);
    // eibispace.de's TLS certificate expires periodically; tolerate cert
    // errors the same way Lyra does for the HamQSL solar feed (it's a
    // public schedule file, not a credentialed endpoint).
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError> &) { reply->ignoreSslErrors(); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, allowHttpFallback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // If https failed outright, try plain http once before giving up.
            if (allowHttpFallback && url.scheme() == QStringLiteral("https")) {
                QUrl httpUrl(url);
                httpUrl.setScheme(QStringLiteral("http"));
                fetch(httpUrl, /*allowHttpFallback=*/false);
                return;
            }
            emit downloadFinished(false, tr("Download failed: %1")
                                             .arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.size() < 1024) {
            emit downloadFinished(false, tr("Download too small — got %1 bytes "
                                            "(check the EiBi site).")
                                             .arg(body.size()));
            return;
        }
        const QString dest = seasonFilePath();
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit downloadFinished(false, tr("Could not write %1")
                                             .arg(QDir::toNativeSeparators(dest)));
            return;
        }
        f.write(body);
        f.close();
        // Clear any stale custom-path override so the fresh file is used.
        QSettings().remove(QString::fromLatin1(kKeyFilePath));
        const bool ok = reload();
        emit downloadFinished(ok, ok ? tr("Updated — %1 entries loaded.")
                                           .arg(entries_.size())
                                     : tr("Downloaded but could not parse the file."));
    });
}

bool EibiStore::importFile(const QString &srcPath) {
    if (srcPath.isEmpty()) return false;
    const QString dest = seasonFilePath();
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QFile::remove(dest);
    if (QFile::copy(srcPath, dest)) {
        // Copied into our swdb folder — use that, drop any custom override.
        QSettings().remove(QString::fromLatin1(kKeyFilePath));
    } else {
        // Couldn't copy (permissions / same path) — point straight at it.
        QSettings().setValue(QString::fromLatin1(kKeyFilePath), srcPath);
    }
    return reload();
}

} // namespace lyra::ui
