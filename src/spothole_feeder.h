// Lyra — SpotHole REST spot feeder.
//
// A standalone DX-spot source: polls the public SpotHole aggregator
// (https://spothole.app/api/v1/spots) on a timer and feeds the normalized
// spots into the source-agnostic SpotStore bus, tagged source="spothole".
// No SDRLogger+ / cluster node required — internet only.  Everything
// downstream (filter, panadapter overlay, console import) reads the bus and
// never knows which feeder produced a spot.
//
// Native C++/Qt6: async QNetworkAccessManager GET, no worker threads, no
// blocking.  Default OFF (opt-in network feature).

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QTimer;

namespace lyra::ipc { class HL2Stream; }

namespace lyra::ui {

class Prefs;
class SpotStore;

class SpotHoleFeeder : public QObject {
    Q_OBJECT
public:
    SpotHoleFeeder(SpotStore *store, Prefs *prefs, lyra::ipc::HL2Stream *stream,
                   QObject *parent = nullptr);

    // --- settings (persisted "spots/spothole/...") ---
    bool    enabled() const         { return enabled_; }
    int     intervalSec() const     { return intervalSec_; }
    int     maxAgeSec() const       { return maxAgeSec_; }
    QString subSource() const       { return subSource_; }   // SpotHole `source` (e.g. "Cluster,POTA"); "" = all
    bool    currentBandOnly() const { return bandOnly_; }
    void setEnabled(bool on);
    void setIntervalSec(int s);
    void setMaxAgeSec(int s);
    void setSubSource(const QString &s);
    void setCurrentBandOnly(bool on);

    // Manual poke (Settings "Refresh now").
    Q_INVOKABLE void refresh();

signals:
    // Last-poll status for the Settings line / status bar (e.g. "12 spots"
    // or an error string).  Never carries spot data.
    void statusChanged(const QString &msg);

private:
    void fetch();
    // The ham band string SpotHole wants for the current dial freq, or ""
    // when the dial is outside the ham bands (→ request all bands).
    QString currentHamBand() const;

    SpotStore             *store_  = nullptr;
    Prefs                 *prefs_  = nullptr;
    lyra::ipc::HL2Stream  *stream_ = nullptr;
    QNetworkAccessManager *nam_    = nullptr;
    QTimer                *timer_  = nullptr;
    bool fetching_ = false;

    bool    enabled_     = false;     // opt-in network feature
    int     intervalSec_ = 30;        // SpotHole's own poll cadence
    int     maxAgeSec_   = 600;       // initial how-far-back window
    QString subSource_   = QStringLiteral("Cluster");
    bool    bandOnly_    = true;      // request only the band you're on
};

} // namespace lyra::ui
