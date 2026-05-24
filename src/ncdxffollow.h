// Lyra — NCDXF beacon auto-follow (ported from old Lyra's
// radio.py ncdxf_follow_* + propagation_panel.py Follow dropdown).
//
// Pick one of the 18 NCDXF International Beacon Project stations and Lyra
// auto-QSYs to whichever of the 5 beacon bands that station is
// transmitting on right now, re-checking every second so it follows the
// station through the 3-minute rotation (20→17→15→12→10 m).  Sets CWU
// and centres the beacon on the panadapter.  Purely a navigation toy /
// propagation tool — drives the Solar/Propagation panel's Follow button.

#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

namespace lyra::ipc { class HL2Stream; }

namespace lyra::ui {

class Prefs;

class NcdxfFollow : public QObject {
    Q_OBJECT
public:
    struct Station { QString call; QString qth; };

    NcdxfFollow(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                QObject *parent = nullptr);

    // The 18 NCDXF stations in rotation order (index = rotation slot 0).
    static const QVector<Station> &stations();

    QString station() const { return station_; }   // "" = follow off
    bool    active() const { return !station_.isEmpty(); }

public slots:
    // Select a station to follow by callsign; "" or unknown = off.
    void setStation(const QString &call);

signals:
    void stationChanged(const QString &call);   // "" when off

private:
    void pump();
    static int indexOf(const QString &call);

    Prefs                 *prefs_  = nullptr;
    lyra::ipc::HL2Stream  *stream_ = nullptr;
    QTimer                *timer_  = nullptr;
    QString                station_;
};

} // namespace lyra::ui
