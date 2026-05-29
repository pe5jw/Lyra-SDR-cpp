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
public:
    // Meter source — what physical quantity the model reads + renders.
    // RX_SMETER is the original / default (WDSP RXA_S_PK in-passband
    // dBm).  PWR/SWR feed from HL2Stream EP6 telemetry (fwd_power /
    // rev_power ADCs).  PA_CURRENT/PA_VOLTS/TEMP are HL2 board
    // telemetry — useful in BOTH MOX states.  ALC/MIC/COMP are TX-side
    // DSP signals that arrive when the WDSP TXA chain lands.  The
    // QML renderers (HorizonArc / PlasmaBar) consume the same Meter
    // context property regardless of source — only the per-tick value
    // computation, scale ticks, text formatting, and zone-threshold
    // semantics change per source.
    enum Source {
        RX_SMETER    = 0,    // WDSP RXA_S_PK — the existing RX behavior
        PWR          = 1,    // forward power (W) — fwd_power ADC + cal
        SWR          = 2,    // (1+rho)/(1-rho) where rho = sqrt(rev/fwd)
        PA_CURRENT   = 3,    // HL2 PA bias current (A)
        PA_VOLTS     = 4,    // HL2 PA supply volts
        TEMP         = 5,    // HL2 board temperature (°C)
        ALC          = 6,    // WDSP TXA_ALC_GAIN (deferred — needs TX DSP)
        MIC          = 7,    // WDSP TXA_MIC_PK   (deferred — needs TX DSP)
        COMP         = 8,    // WDSP TXA_LVLR_GAIN (deferred — needs TX DSP)
    };
    Q_ENUM(Source)

private:
    // One NOTIFY for the fast-changing values — QML repaints on `updated`.
    Q_PROPERTY(double level    READ level    NOTIFY updated)
    Q_PROPERTY(double peak     READ peak     NOTIFY updated)
    // Max-hold "high-water mark" — a second marker that latches the
    // highest level seen, holds for maxHoldMs, then eases down gently
    // (slower than the fast peak pip).  Distinct marker in the renderers.
    Q_PROPERTY(double maxPeak  READ maxPeak  NOTIFY updated)
    Q_PROPERTY(bool maxPeakEnabled READ maxPeakEnabled WRITE setMaxPeakEnabled
               NOTIFY maxPeakCfgChanged)
    Q_PROPERTY(int  maxHoldMs READ maxHoldMs WRITE setMaxHoldMs
               NOTIFY maxPeakCfgChanged)
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
    // Peak-hold dwell time in ms before the peak marker starts to decay
    // (operator-tunable in Settings → Meter; persisted).
    Q_PROPERTY(int peakHoldMs READ peakHoldMs WRITE setPeakHoldMs
               NOTIFY peakHoldChanged)
    // PWR calibration knobs (task #34).  pwrRatedMaxW sets where the
    // red zone starts on the meter face (== operator's expected max
    // forward power: ~5 W for a bare HL2+ on-board PA, ~100 W or more
    // with an external amp).  pwrCalScale multiplies the provisional
    // fwd_power ADC→W formula in HL2Stream so the displayed watts can
    // be trimmed against an external watt-meter reading.  Both persisted.
    Q_PROPERTY(double pwrRatedMaxW READ pwrRatedMaxW WRITE setPwrRatedMaxW
               NOTIFY pwrCalChanged)
    Q_PROPERTY(double pwrCalScale  READ pwrCalScale  WRITE setPwrCalScale
               NOTIFY pwrCalChanged)
    // Active source — what the renderer is showing RIGHT NOW.  Derived
    // from the operator's RX/TX preferences and the live wire MOX bit:
    //   * moxActive=false → source = rxSource
    //   * moxActive=true  → source = txSource
    // The setter (used by click-to-cycle, task #35) updates whichever
    // preference applies to the current MOX state — so the operator's
    // choice persists into the right slot.  Auto-swap on MOX edge is
    // wired in the ctor via HL2Stream::moxActiveChanged.
    Q_PROPERTY(int source READ source WRITE setSource NOTIFY sourceChanged)
    // Operator's RX-state and TX-state source preferences — what to
    // show at rest (rxSource, default RX_SMETER) and what to swap to
    // when the wire MOX bit settles (txSource, default PWR).  Each is
    // persisted independently; Settings → Meter exposes both via
    // dropdowns.  Changing either preference takes effect immediately
    // if the current MOX state matches that preference's slot.
    Q_PROPERTY(int rxSource READ rxSource WRITE setRxSource
               NOTIFY rxSourceChanged)
    Q_PROPERTY(int txSource READ txSource WRITE setTxSource
               NOTIFY txSourceChanged)

public:
    explicit MeterModel(lyra::ipc::HL2Stream *stream,
                        lyra::dsp::WdspEngine *wdsp,
                        QObject *parent = nullptr);

    double  level()    const { return level_; }
    double  peak()     const { return peak_; }
    double  maxPeak()  const { return maxPeak_; }
    bool    maxPeakEnabled() const { return maxPeakEnabled_; }
    int     maxHoldMs() const { return maxHoldMs_; }
    void    setMaxPeakEnabled(bool on);
    void    setMaxHoldMs(int ms);
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
    int  peakHoldMs() const { return peakHoldMs_; }
    void setPeakHoldMs(int ms);
    int  source() const { return int(source_); }
    void setSource(int s);
    int  rxSource() const { return int(rxSource_); }
    int  txSource() const { return int(txSource_); }
    void setRxSource(int s);
    void setTxSource(int s);
    double pwrRatedMaxW() const { return pwrRatedMaxW_; }
    double pwrCalScale()  const { return pwrCalScale_; }
    void setPwrRatedMaxW(double w);
    void setPwrCalScale(double s);

    // Tick marks for the scale: list of { pos: 0..1, label: "9"/"+20", major: bool }.
    Q_INVOKABLE QVariantList tickMarks() const;

signals:
    void updated();
    void styleChanged();
    void calChanged();
    void peakHoldChanged();
    void maxPeakCfgChanged();
    void sourceChanged();
    void rxSourceChanged();
    void txSourceChanged();
    void pwrCalChanged();

private:
    void tick();
    // Per-source tick computations.  Each fills the model's Q_PROPERTY
    // fields (level_/peak_/maxPeak_/glow_/text_/dbmText_/snrText_/
    // history_/noiseLevel_/floorDbm_/ceilDbm_/s9Dbm_) appropriately for
    // that source's scale + semantics.  computeSMeter is the existing
    // RX behavior (extracted unchanged from the old tick() body); other
    // computes land in subsequent commits.
    void computeSMeter();
    void computePwr();
    void computeSwr();
    double normForDbm(double dbm) const;
    void   updateScale();              // pick HF/VHF endpoints from the VFO freq
    QString sLabel(double dbm) const;  // Thetis SMeterFromDBM table
    // PWR cal: operator's reference anchor (10m / dummy / full TUN
    // reading on a known watt-meter) maps the provisional fwd_power ADC
    // -> watts curve to a true scalar.  Single global cal for v0.2.x
    // (per-band 3-point cal is a polish item).  Persisted under
    // meter/pwrCalScale (default 1.0 = use the provisional formula
    // as-is) + meter/pwrRatedMaxW (default 5.0 W = HL2+ on-board PA).
    double pwrCalScale_  = 1.0;     // applied to fwdPowerW() output
    double pwrRatedMaxW_ = 5.0;     // danger-zone (red) starts here
    double pwrScaleMaxW_ = 10.0;    // full-scale watts (== 2 * rated max)
    // Source-agnostic "danger zone start" position (0..1 along the
    // scale).  S-meter computes from dBm math via the existing path;
    // PWR/SWR/etc. write directly.  normAtS9() Q_PROPERTY returns
    // this for non-SMETER sources (kept the historical name so the
    // QML renderers' zone-coloring logic works unchanged for all
    // sources — only the threshold's meaning differs per source).
    double normDanger_ = 0.0;

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
    double maxPeak_ = 0.0;     // max-hold "high-water mark" normalized
    double glow_  = 0.0;       // afterglow normalized
    int    holdCtr_ = 0;       // peak-hold dwell counter
    int    maxHoldCtr_ = 0;    // max-hold dwell counter
    double dispDbm_ = -140.0;  // smoothed dBm for the readout
    double noiseFloorDbm_ = -140.0;  // rolling-minimum noise floor (dBm)
    double noiseLevel_ = 0.0;  // floor position on the scale (0..1)
    QString snrText_ = QStringLiteral("—");

    double  calDb_ = 0.0;
    int     peakHoldMs_    = 800;   // dwell before decay (operator-tunable)
    int     peakHoldTicks_ = 16;    // = peakHoldMs_ / tick interval
    bool    maxPeakEnabled_ = true; // show the max-hold high-water marker
    int     maxHoldMs_      = 3000; // max-hold dwell (operator-tunable)
    int     maxHoldTicks_   = 60;   // = maxHoldMs_ / tick interval
    int     style_ = 0;
    Source  source_ = RX_SMETER;     // currently-displayed source (derived)
    Source  rxSource_ = RX_SMETER;   // RX-state preference (persisted)
    Source  txSource_ = PWR;         // TX-state preference (persisted)
    QString text_    = QStringLiteral("S0");
    QString dbmText_ = QStringLiteral("—");

    QVariantList     history_;
    std::deque<double> hist_;
};

} // namespace lyra::ui
