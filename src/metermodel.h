// Lyra — RX signal-strength meter model.
//
// Feeds the meter panels (Horizon Arc / Plasma Bar).  Reads the RX1
// baseband IQ RMS level (HL2Stream::rx1DbFs, pre-DSP so AGC doesn't
// flatten it), applies an operator calibration trim to land it on the
// S-unit scale, and derives everything the QML renderers need:
//   • level  — current value, 0..1 along the S1…S9+60 scale (smoothed)
//   • peak   — peak-hold marker (holds ~0.8 s then decays)
//   • glow   — phosphor afterglow (trails the DROP, fast decay)
//   • text   — "S9+20" style readout
//   • dbmText— calibrated dBm readout
//   • history— recent level ring for the arc's trailing sparkline
//
// Today the only source is the RX S-meter.  PWR/SWR/ALC/MIC/PA/Temp
// slot in as sibling sources (with click-to-cycle) when TX + HL2
// telemetry land — so there are no inert placeholders now.

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <deque>

namespace lyra::ipc { class HL2Stream; }
namespace lyra::dsp { class WdspEngine; }

namespace lyra::ui {

class MeterModel : public QObject {
    Q_OBJECT
    // One NOTIFY for the fast-changing values — QML repaints on `updated`.
    Q_PROPERTY(double level    READ level    NOTIFY updated)
    Q_PROPERTY(double peak     READ peak     NOTIFY updated)
    Q_PROPERTY(double glow     READ glow     NOTIFY updated)
    Q_PROPERTY(QString text    READ text     NOTIFY updated)
    Q_PROPERTY(QString dbmText READ dbmText  NOTIFY updated)
    Q_PROPERTY(QVariantList history READ history NOTIFY updated)
    // Noise-floor marker (0..1 on the scale) + signal-above-noise readout.
    // The floor is a slow rolling-minimum of the level; SNR = signal-floor.
    Q_PROPERTY(double noiseLevel READ noiseLevel NOTIFY updated)
    Q_PROPERTY(QString snrText   READ snrText    NOTIFY updated)
    // The normalized position of the S9 boundary — renderers paint the
    // over-S9 region red.  Tracks the HF/VHF scale (S9 shifts at 30 MHz).
    Q_PROPERTY(double normAtS9 READ normAtS9 NOTIFY updated)
    // Operator-selectable visual style: 0 = Horizon Arc (default), 1 = Plasma Bar.
    Q_PROPERTY(int style READ style WRITE setStyle NOTIFY styleChanged)
    // dBFS→dBm calibration trim (operator-tunable; persisted).
    Q_PROPERTY(double calDb READ calDb WRITE setCalDb NOTIFY calChanged)

public:
    explicit MeterModel(lyra::ipc::HL2Stream *stream,
                        lyra::dsp::WdspEngine *wdsp,
                        QObject *parent = nullptr);

    double  level()    const { return level_; }
    double  peak()     const { return peak_; }
    double  glow()     const { return glow_; }
    double  noiseLevel() const { return noiseLevel_; }
    QString snrText()  const { return snrText_; }
    QString text()     const { return text_; }
    QString dbmText()  const { return dbmText_; }
    QVariantList history() const { return history_; }
    double  normAtS9() const;

    int  style() const { return style_; }
    void setStyle(int s);
    double calDb() const { return calDb_; }
    void setCalDb(double d);

    // Tick marks for the scale: list of { pos: 0..1, label: "9"/"+20", major: bool }.
    Q_INVOKABLE QVariantList tickMarks() const;

signals:
    void updated();
    void styleChanged();
    void calChanged();

private:
    void tick();
    double normForDbm(double dbm) const;
    void   updateScale();              // pick HF/VHF endpoints from the VFO freq
    QString sLabel(double dbm) const;  // Thetis SMeterFromDBM table

    lyra::ipc::HL2Stream  *stream_ = nullptr;
    lyra::dsp::WdspEngine *wdsp_   = nullptr;
    QTimer timer_;

    // Active scale endpoints (recomputed each tick from the VFO freq;
    // S9 = -73 dBm below 30 MHz, -93 dBm above — Thetis S9Frequency).
    double floorDbm_ = -124.0;
    double ceilDbm_  = -13.0;
    double s9Dbm_    = -73.0;
    bool   aboveS9_  = false;

    double level_ = 0.0;       // smoothed normalized value
    double peak_  = 0.0;       // peak-hold normalized
    double glow_  = 0.0;       // afterglow normalized
    int    holdCtr_ = 0;       // peak-hold dwell counter
    double dispDbm_ = -140.0;  // smoothed dBm for the readout
    double noiseFloorDbm_ = -140.0;  // rolling-minimum noise floor (dBm)
    double noiseLevel_ = 0.0;  // floor position on the scale (0..1)
    QString snrText_ = QStringLiteral("—");

    double  calDb_ = 0.0;
    int     style_ = 0;
    QString text_    = QStringLiteral("S0");
    QString dbmText_ = QStringLiteral("—");

    QVariantList     history_;
    std::deque<double> hist_;
};

} // namespace lyra::ui
