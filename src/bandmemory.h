// Lyra — per-band settings memory (ported from old Lyra's band_memory).
//
// Remembers, per amateur band, the operator's demod MODE, panadapter dB
// min/max, and waterfall dB min/max.  As you move across a band edge it
// restores that band's saved values; while you're on a band, any change
// is saved live to it.  Auto-scale on/off and RX bandwidth stay global /
// per-mode (matching old Lyra — bandwidth is already per-mode in Prefs).
//
// Pure C++: watches the stream's RX1 frequency, maps it to a band via the
// amateur band table, and drives the shared Prefs (which the panadapter /
// mode panels are bound to).  Persisted under QSettings "band_mem/<band>/*".

#pragma once

#include <QObject>
#include <QString>

namespace lyra::ipc { class HL2Stream; }

namespace lyra::ui {

class Prefs;

class BandMemory : public QObject {
    Q_OBJECT
public:
    BandMemory(Prefs *prefs, lyra::ipc::HL2Stream *stream,
               QObject *parent = nullptr);

    // Last frequency the operator was on in <band> (Hz), or 0 if none —
    // the Band panel buttons use this to return you to where you were.
    Q_INVOKABLE int freqFor(const QString &band) const;

private:
    void onFreqChanged();          // band-edge crossing → restore new band
    void saveCurrent();            // live-save the current band on a change
    void applyBand(const QString &band);
    static QString bandNameFor(int hz);

    Prefs                *prefs_  = nullptr;
    lyra::ipc::HL2Stream *stream_ = nullptr;
    QString               currentBand_;     // "" = none/out-of-band
    bool                  applying_ = false; // guard: don't re-save during restore
};

} // namespace lyra::ui
