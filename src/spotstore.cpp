// Lyra — DX-cluster spot store.  See spotstore.h.

#include "spotstore.h"

#include "hl2_stream.h"
#include "prefs.h"
#include "wdsp_engine.h"

#include <QColor>
#include <QDateTime>
#include <QSettings>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>

namespace lyra::ui {

namespace {
constexpr auto kKeyShow = "spots/show";
constexpr auto kKeyMax  = "spots/max";
constexpr auto kKeyLife = "spots/lifetime_sec";
constexpr auto kKeyHlOwn = "spots/highlight_own";
constexpr auto kKeyHlCol = "spots/highlight_color";
constexpr auto kKeyNotify = "spots/notify_own";
constexpr auto kKeyCooldown = "spots/notify_cooldown_min";
constexpr auto kKeyFlashNew = "spots/flash_new";
constexpr auto kKeyFlashCol = "spots/flash_color";
constexpr auto kKeyFlashSec = "spots/flash_sec";
}

SpotStore::SpotStore(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                     lyra::dsp::WdspEngine *engine, QObject *parent)
    : QObject(parent), prefs_(prefs), stream_(stream), engine_(engine) {
    QSettings s;
    show_        = s.value(QString::fromLatin1(kKeyShow), true).toBool();
    max_         = s.value(QString::fromLatin1(kKeyMax), 200).toInt();
    lifetimeSec_ = s.value(QString::fromLatin1(kKeyLife), 0).toInt();
    highlightOwn_   = s.value(QString::fromLatin1(kKeyHlOwn), true).toBool();
    highlightColor_ = s.value(QString::fromLatin1(kKeyHlCol),
                              QStringLiteral("#ff4081")).toString();
    notifyOwn_         = s.value(QString::fromLatin1(kKeyNotify), false).toBool();
    notifyCooldownMin_ = s.value(QString::fromLatin1(kKeyCooldown), 10).toInt();
    flashNew_   = s.value(QString::fromLatin1(kKeyFlashNew), false).toBool();
    flashColor_ = s.value(QString::fromLatin1(kKeyFlashCol),
                          QStringLiteral("#ffeb3b")).toString();
    flashSec_   = qBound(1, s.value(QString::fromLatin1(kKeyFlashSec), 8).toInt(), 60);
    tStart_ = QDateTime::currentMSecsSinceEpoch();

    ageTimer_ = new QTimer(this);
    ageTimer_->setInterval(5000);
    connect(ageTimer_, &QTimer::timeout, this, &SpotStore::onAgeTick);
    if (lifetimeSec_ > 0) ageTimer_->start();

    // Flash repaint pulse — runs only while a spot is inside the flash
    // window; ~3 Hz is smooth enough for the eye and cheap (spotsInSpan
    // returns only the handful of in-span spots).  Self-stops once the
    // window passes (onFlashTick).
    flashTimer_ = new QTimer(this);
    flashTimer_->setInterval(330);
    connect(flashTimer_, &QTimer::timeout, this, &SpotStore::onFlashTick);
}

int SpotStore::indexOf(const QString &call) const {
    for (int i = 0; i < spots_.size(); ++i)
        if (spots_[i].call.compare(call, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

void SpotStore::addSpot(const QString &call, const QString &mode, qint64 freqHz,
                        quint32 argb, const QString &text) {
    if (call.trimmed().isEmpty() || freqHz <= 0) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int idx = indexOf(call);
    Spot sp;
    sp.call = call.trimmed();
    sp.mode = mode.trimmed();
    sp.freqHz = freqHz;
    sp.argb = argb ? argb : 0xFFFFD700u;
    sp.text = text.trimmed();
    sp.country = dxcc_.isoOf(sp.call);     // DXCC country abbreviation
    sp.added = now;
    if (idx >= 0) spots_[idx] = sp;     // update in place (same key)
    else          spots_.append(sp);
    enforceCap();

    // Toast when the operator's own callsign is spotted — rate-limited by
    // the re-notify cooldown so a busy cluster can't spam.
    if (notifyOwn_ && prefs_) {
        const QString own = prefs_->callsign().trimmed().toUpper();
        if (!own.isEmpty() && sp.call.toUpper() == own) {
            const qint64 gapMs = qint64(qMax(0, notifyCooldownMin_)) * 60000;
            if (now - lastNotify_ >= gapMs) {
                lastNotify_ = now;
                emit ownCallSpotted(sp.call, sp.mode, sp.freqHz, sp.text);
            }
        }
    }
    // #172 — start the flash pulse so the just-added spot decays out of
    // flashColor_ on its own (idempotent: start() on a running timer is
    // a no-op).  Skip self-spots — they already have the own-call ★ +
    // highlight colour, which takes visual priority over a flash.
    if (flashNew_ && flashTimer_ && !flashTimer_->isActive()) {
        const QString own = prefs_ ? prefs_->callsign().trimmed().toUpper()
                                   : QString();
        if (own.isEmpty() || sp.call.toUpper() != own
            || !(highlightOwn_))
            flashTimer_->start();
    }
    emit changed();
}

void SpotStore::deleteSpot(const QString &call) {
    const int idx = indexOf(call);
    if (idx < 0) return;
    spots_.removeAt(idx);
    emit changed();
}

void SpotStore::clearAll() {
    if (spots_.isEmpty()) return;
    spots_.clear();
    emit changed();
}

void SpotStore::enforceCap() {
    if (max_ <= 0) return;
    // Drop oldest (front-most by add time) until within cap.
    while (spots_.size() > max_) {
        int oldest = 0;
        for (int i = 1; i < spots_.size(); ++i)
            if (spots_[i].added < spots_[oldest].added) oldest = i;
        spots_.removeAt(oldest);
    }
}

void SpotStore::onAgeTick() {
    if (lifetimeSec_ <= 0) return;
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch()
                          - qint64(lifetimeSec_) * 1000;
    bool removed = false;
    for (int i = spots_.size() - 1; i >= 0; --i)
        if (spots_[i].added < cutoff) { spots_.removeAt(i); removed = true; }
    if (removed) emit changed();
}

bool SpotStore::anyFlashing(qint64 nowMs) const {
    if (!flashNew_) return false;
    const qint64 flashMs = qint64(flashSec_) * 1000;
    for (const Spot &sp : spots_)
        if (nowMs - sp.added < flashMs) return true;
    return false;
}

void SpotStore::onFlashTick() {
    // Re-query forces the panadapter to re-read each spot's `flash` flag,
    // so spots whose window just lapsed settle to their normal colour.
    // Stop pulsing once nothing is flashing (idle = zero cost).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!anyFlashing(now)) { flashTimer_->stop(); return; }
    emit changed();
}

QString SpotStore::lyraModeFor(const QString &tciMode, qint64 freqHz) {
    const QString m = tciMode.trimmed().toUpper();
    const bool high = freqHz >= 10000000;
    if (m == QStringLiteral("CW"))  return high ? QStringLiteral("CWU") : QStringLiteral("CWL");
    if (m == QStringLiteral("SSB")) return high ? QStringLiteral("USB") : QStringLiteral("LSB");
    if (m == QStringLiteral("USB") || m == QStringLiteral("LSB")
        || m == QStringLiteral("CWU") || m == QStringLiteral("CWL")
        || m == QStringLiteral("AM")  || m == QStringLiteral("SAM")
        || m == QStringLiteral("DSB") || m == QStringLiteral("FM")
        || m == QStringLiteral("DIGU") || m == QStringLiteral("DIGL"))
        return m;
    if (m == QStringLiteral("NFM") || m == QStringLiteral("WFM"))
        return QStringLiteral("FM");
    if (m == QStringLiteral("RTTY-R") || m == QStringLiteral("DIGL"))
        return QStringLiteral("DIGL");
    if (m == QStringLiteral("FT8") || m == QStringLiteral("FT4")
        || m == QStringLiteral("RTTY") || m == QStringLiteral("PSK")
        || m == QStringLiteral("DATA") || m == QStringLiteral("DIG"))
        return QStringLiteral("DIGU");
    return QString();   // unknown → leave current mode
}

void SpotStore::activate(const QString &call) {
    const int idx = indexOf(call);
    if (idx < 0) return;
    const Spot sp = spots_[idx];
    const QString lyraMode = lyraModeFor(sp.mode, sp.freqHz);
    if (prefs_ && !lyraMode.isEmpty()) prefs_->setMode(lyraMode);
    // A spot's frequency is the CARRIER.  In CW the DDS (tuned centre) must
    // sit pitch Hz off the carrier so the operator hears the signal at their
    // CW pitch instead of zero-beat: DDS = carrier − (VFO−DDS offset).
    // Compute the offset for the mode we're switching TO (the Prefs→engine
    // mode binding updates asynchronously, so don't rely on the live mode).
    qint64 dds = sp.freqHz;
    if (engine_ && !lyraMode.isEmpty())
        dds -= engine_->cwMarkerOffsetForMode(lyraMode);
    if (stream_) stream_->setRx1FreqHz(quint32(dds));
    // Echo the original CARRIER frequency so the logger logs the right QSO.
    emit spotActivated(sp.call, sp.mode, sp.freqHz, sp.argb);
}

QVariantList SpotStore::spotsInSpan(double centerHz, double spanHz) const {
    QVariantList out;
    if (!show_ || spanHz <= 0) return out;
    const double lo = centerHz - spanHz / 2.0;
    const double hi = centerHz + spanHz / 2.0;
    // Gather the in-span spots and sort by frequency so the panadapter's
    // row-stagger places frequency-adjacent spots on different rows
    // (declutters callsigns that sit on top of each other).
    QVector<const Spot *> hits;
    for (const Spot &sp : spots_)
        if (sp.freqHz >= lo && sp.freqHz <= hi) hits.append(&sp);
    std::sort(hits.begin(), hits.end(),
              [](const Spot *a, const Spot *b) { return a->freqHz < b->freqHz; });
    const QString own = prefs_ ? prefs_->callsign().trimmed().toUpper() : QString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 flashMs = qint64(flashSec_) * 1000;
    for (const Spot *sp : hits) {
        const bool mine = highlightOwn_ && !own.isEmpty()
                          && sp->call.toUpper() == own;
        // Flash a freshly-arrived spot — but never override the own-call
        // highlight (mine), which is the stronger signal.
        const bool flash = flashNew_ && !mine && (nowMs - sp->added) < flashMs;
        QVariantMap m;
        m[QStringLiteral("call")]    = sp->call;          // raw (for activate/echo)
        m[QStringLiteral("country")] = sp->country;       // ISO-2 or ""
        m[QStringLiteral("label")]   = sp->country.isEmpty()
                                       ? sp->call
                                       : sp->country + QLatin1Char(' ') + sp->call;
        m[QStringLiteral("freqHz")]  = double(sp->freqHz);
        m[QStringLiteral("color")]   = mine ? highlightColor_
                                            : QColor::fromRgba(sp->argb).name(QColor::HexArgb);
        m[QStringLiteral("mine")]    = mine;
        m[QStringLiteral("flash")]   = flash;
        m[QStringLiteral("flashColor")] = flashColor_;
        m[QStringLiteral("mode")]    = sp->mode;
        m[QStringLiteral("text")]    = sp->text;
        out.append(m);
    }
    return out;
}

void SpotStore::setShowSpots(bool on) {
    if (show_ == on) return;
    show_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyShow), on);
    emit changed();
}
void SpotStore::setMaxSpots(int n) {
    n = qBound(1, n, 5000);
    if (max_ == n) return;
    max_ = n;
    QSettings().setValue(QString::fromLatin1(kKeyMax), n);
    enforceCap();
    emit changed();
}
void SpotStore::setLifetimeSec(int s) {
    s = qMax(0, s);
    if (lifetimeSec_ == s) return;
    lifetimeSec_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyLife), s);
    if (lifetimeSec_ > 0) ageTimer_->start();
    else                  ageTimer_->stop();
}
void SpotStore::setHighlightOwn(bool on) {
    if (highlightOwn_ == on) return;
    highlightOwn_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyHlOwn), on);
    emit changed();
}
void SpotStore::setHighlightColor(const QString &hex) {
    if (hex.isEmpty() || highlightColor_ == hex) return;
    highlightColor_ = hex;
    QSettings().setValue(QString::fromLatin1(kKeyHlCol), hex);
    emit changed();
}
void SpotStore::setNotifyOwn(bool on) {
    if (notifyOwn_ == on) return;
    notifyOwn_ = on;
    lastNotify_ = 0;   // allow an immediate first notification
    QSettings().setValue(QString::fromLatin1(kKeyNotify), on);
}
void SpotStore::setNotifyCooldownMin(int min) {
    min = qBound(0, min, 240);
    if (notifyCooldownMin_ == min) return;
    notifyCooldownMin_ = min;
    QSettings().setValue(QString::fromLatin1(kKeyCooldown), min);
}
void SpotStore::setFlashNew(bool on) {
    if (flashNew_ == on) return;
    flashNew_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyFlashNew), on);
    // Turning on while recent spots exist → pulse immediately; turning
    // off → the next onFlashTick sees !flashNew_ via anyFlashing and
    // stops.  Either way re-query so the change shows at once.
    if (flashNew_ && flashTimer_ && !flashTimer_->isActive()
        && anyFlashing(QDateTime::currentMSecsSinceEpoch()))
        flashTimer_->start();
    emit changed();
}
void SpotStore::setFlashColor(const QString &hex) {
    if (hex.isEmpty() || flashColor_ == hex) return;
    flashColor_ = hex;
    QSettings().setValue(QString::fromLatin1(kKeyFlashCol), hex);
    emit changed();
}
void SpotStore::setFlashSec(int s) {
    s = qBound(1, s, 60);
    if (flashSec_ == s) return;
    flashSec_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyFlashSec), s);
    emit changed();
}

} // namespace lyra::ui
