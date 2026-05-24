// Lyra — per-band settings memory.  See bandmemory.h.

#include "bandmemory.h"

#include "bands.h"
#include "hl2_stream.h"
#include "prefs.h"

#include <QSettings>

namespace lyra::ui {

namespace {
constexpr auto kPfx = "band_mem/";   // band_mem/<band>/<field>
}

BandMemory::BandMemory(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                       QObject *parent)
    : QObject(parent), prefs_(prefs), stream_(stream) {
    if (stream_)
        connect(stream_, &lyra::ipc::HL2Stream::rx1FreqChanged,
                this, &BandMemory::onFreqChanged);
    if (prefs_) {
        // Live-save the current band whenever a remembered setting changes
        // (so values are kept even if the operator never leaves the band).
        // applying_ guards out the changes WE make while restoring a band.
        auto live = [this]() { saveCurrent(); };
        connect(prefs_, &Prefs::modeChanged,           this, live);
        connect(prefs_, &Prefs::dbMinChanged,          this, live);
        connect(prefs_, &Prefs::dbMaxChanged,          this, live);
        connect(prefs_, &Prefs::waterfallDbMinChanged, this, live);
        connect(prefs_, &Prefs::waterfallDbMaxChanged, this, live);
    }
}

QString BandMemory::bandNameFor(int hz) {
    const int idx = lyra::bandIndexForFreq(hz);
    if (idx < 0) return QString();
    return QString::fromLatin1(lyra::amateurBands()[std::size_t(idx)].name);
}

void BandMemory::onFreqChanged() {
    if (applying_ || !stream_) return;
    const int hz = int(stream_->rx1FreqHz());
    const QString name = bandNameFor(hz);
    if (name != currentBand_) {           // crossed a band edge
        currentBand_ = name;
        if (!name.isEmpty()) applyBand(name);   // restore mode/dB ranges
    }
    // Remember this band's last frequency so the Band buttons can return
    // to where you were (see freqFor()).
    if (!name.isEmpty())
        QSettings().setValue(QString::fromLatin1(kPfx) + name +
                             QStringLiteral("/freq"), hz);
}

int BandMemory::freqFor(const QString &band) const {
    return QSettings().value(QString::fromLatin1(kPfx) + band +
                             QStringLiteral("/freq"), 0).toInt();
}

void BandMemory::saveCurrent() {
    if (applying_ || !prefs_ || currentBand_.isEmpty()) return;
    QSettings s;
    const QString p = QString::fromLatin1(kPfx) + currentBand_ + QLatin1Char('/');
    s.setValue(p + QStringLiteral("mode"),  prefs_->mode());
    s.setValue(p + QStringLiteral("dbMin"), prefs_->dbMin());
    s.setValue(p + QStringLiteral("dbMax"), prefs_->dbMax());
    s.setValue(p + QStringLiteral("wfMin"), prefs_->waterfallDbMin());
    s.setValue(p + QStringLiteral("wfMax"), prefs_->waterfallDbMax());
}

void BandMemory::applyBand(const QString &band) {
    if (!prefs_) return;
    QSettings s;
    const QString p = QString::fromLatin1(kPfx) + band + QLatin1Char('/');
    if (!s.contains(p + QStringLiteral("mode"))) return;   // never visited
    applying_ = true;                     // don't re-save these as user edits
    prefs_->setMode(s.value(p + QStringLiteral("mode")).toString());
    prefs_->setDbMin(s.value(p + QStringLiteral("dbMin")).toDouble());
    prefs_->setDbMax(s.value(p + QStringLiteral("dbMax")).toDouble());
    prefs_->setWaterfallDbMin(s.value(p + QStringLiteral("wfMin")).toDouble());
    prefs_->setWaterfallDbMax(s.value(p + QStringLiteral("wfMax")).toDouble());
    applying_ = false;
}

} // namespace lyra::ui
