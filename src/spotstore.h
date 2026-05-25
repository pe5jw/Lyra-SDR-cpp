// Lyra — DX-cluster spot store (TCI spots).
//
// Holds the spots pushed by a TCI client (SDRLogger+, cluster apps):
// each { callsign, mode, freqHz, ARGB colour, text }.  The panadapter
// draws them as colored callsign markers; clicking one tunes there and
// echoes `spot_activated` back to the TCI clients (so the logger can
// log the QSO).  Spots optionally age out after a lifetime, and the
// bank is capped (oldest dropped) to keep the display uncluttered.

#pragma once

#include "dxcc.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

class QTimer;

namespace lyra::ipc { class HL2Stream; }
namespace lyra::dsp { class WdspEngine; }

namespace lyra::ui {

class Prefs;

class SpotStore : public QObject {
    Q_OBJECT
public:
    SpotStore(Prefs *prefs, lyra::ipc::HL2Stream *stream,
              lyra::dsp::WdspEngine *engine, QObject *parent = nullptr);

    // --- TCI inbound ---
    void addSpot(const QString &call, const QString &mode, qint64 freqHz,
                 quint32 argb, const QString &text);
    void deleteSpot(const QString &call);
    void clearAll();

    // --- QML / panadapter ---
    // Spots within [center±span/2]: { call, freqHz, color (#AARRGGBB),
    // mode, text }.  Empty when display is disabled.
    Q_INVOKABLE QVariantList spotsInSpan(double centerHz, double spanHz) const;
    // Click-to-tune: tune to <call>'s spot (mode + freq) and emit
    // spotActivated so the TCI layer can echo it to clients.
    Q_INVOKABLE void activate(const QString &call);

    // --- settings (persisted "spots/...") ---
    bool showSpots() const   { return show_; }
    int  maxSpots() const    { return max_; }
    int  lifetimeSec() const { return lifetimeSec_; }
    bool highlightOwn() const     { return highlightOwn_; }
    QString highlightColor() const { return highlightColor_; }
    bool notifyOwn() const        { return notifyOwn_; }
    int  notifyCooldownMin() const { return notifyCooldownMin_; }
    void setShowSpots(bool on);
    void setMaxSpots(int n);
    void setLifetimeSec(int s);
    void setHighlightOwn(bool on);
    void setHighlightColor(const QString &hex);
    void setNotifyOwn(bool on);
    void setNotifyCooldownMin(int min);

signals:
    void changed();   // bank changed → panadapter re-queries
    void spotActivated(const QString &call, const QString &mode,
                       qint64 freqHz, quint32 argb);
    // The operator's own callsign was just spotted (rate-limited by the
    // re-notify cooldown).  MainWindow turns this into a toast.
    void ownCallSpotted(const QString &call, const QString &mode,
                        qint64 freqHz, const QString &text);

private:
    struct Spot {
        QString call, mode, text, country;   // country = ISO-2 (DXCC), "" if unknown
        qint64  freqHz = 0;
        quint32 argb   = 0xFFFFD700;   // gold default
        qint64  added  = 0;            // ms since store start (for lifetime)
    };
    int  indexOf(const QString &call) const;
    void enforceCap();
    void onAgeTick();
    static QString lyraModeFor(const QString &tciMode, qint64 freqHz);

    Prefs                 *prefs_  = nullptr;
    lyra::ipc::HL2Stream  *stream_ = nullptr;
    lyra::dsp::WdspEngine *engine_ = nullptr;
    DxccLookup             dxcc_;
    QVector<Spot>         spots_;
    QTimer               *ageTimer_ = nullptr;
    qint64                tStart_ = 0;

    bool show_        = true;
    int  max_         = 200;
    int  lifetimeSec_ = 0;     // 0 = never expire
    bool    highlightOwn_   = true;
    QString highlightColor_ = QStringLiteral("#ff4081");   // magenta-pink
    bool    notifyOwn_         = false;   // toast when my call is spotted
    int     notifyCooldownMin_ = 10;      // re-notify no sooner than this
    qint64  lastNotify_        = 0;       // ms epoch of the last toast
};

} // namespace lyra::ui
