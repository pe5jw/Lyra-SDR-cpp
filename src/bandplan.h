// Lyra — amateur band-plan data (ported from old Lyra's band_plan.py).
//
// Per-region sub-band segments (CW / DIG / SSB / FM …), band edges, and
// "watering-hole" landmarks (FT8 / FT4 / WSPR / PSK + NCDXF beacons).
// Purely advisory — the HL2 is unlocked; this drives the panadapter's
// top-strip overlay and out-of-band hints.  Region comes from Prefs
// (US / IARU_R1 / IARU_R3 / NONE).  Exposed to QML as `BandPlan`.
//
// The DSP/protocol side never reads this — it's a navigation aid only.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

namespace lyra::ui {

class Prefs;

class BandPlan : public QObject {
    Q_OBJECT
    // Mirrors Prefs.bandPlanRegion; QML overlay bindings depend on this
    // so they re-query when the operator changes region.
    Q_PROPERTY(QString region READ region NOTIFY regionChanged)
public:
    explicit BandPlan(Prefs *prefs, QObject *parent = nullptr);

    QString region() const;

    // Segments intersecting the visible window [center±span/2].  Each
    // entry: { lo, hi (Hz, clipped), color (#RRGGBB), label }.
    Q_INVOKABLE QVariantList segments(double centerHz, double spanHz) const;
    // Band edges (low/high of each allocation) in view.
    // Each: { freq, name }.
    Q_INVOKABLE QVariantList edges(double centerHz, double spanHz) const;
    // Watering-hole landmarks in view, gated by group.
    // Each: { freq, label, mode, beacon (bool) }.
    Q_INVOKABLE QVariantList landmarks(double centerHz, double spanHz,
                                       bool showDigital, bool showBeacons) const;
    // NCDXF International Beacon Project: the station transmitting RIGHT
    // NOW on the given beacon frequency, as "<call> — <QTH>", or "" if
    // the freq isn't an NCDXF beacon.  Computed from the 18-station /
    // 10-second-slot / 3-minute rotation.  Used in the beacon tooltip.
    Q_INVOKABLE QString ncdxfStation(double freqHz) const;
    // Name of the amateur band containing freqHz ("40m"…), or "" if the
    // freq is outside every allocation in the current region (or NONE).
    Q_INVOKABLE QString bandContaining(double freqHz) const;
    // The 40 US CB channels visible in the window, each { freq, label }.
    // Empty unless the 11m/CB band is enabled (Settings → Hardware).
    Q_INVOKABLE QVariantList cbChannels(double centerHz, double spanHz) const;

signals:
    void regionChanged();

private:
    // Optional country override (Prefs.bandPlanCountry; "AUTO" = none).
    QString country() const;

    Prefs *prefs_ = nullptr;
};

} // namespace lyra::ui
