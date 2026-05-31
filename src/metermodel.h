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
    // PWR meter MAX-window hold time (operator bench 2026-05-31 PM:
    // "still seems SLOW to react" after the initial 500 ms -> 3 s
    // bump — operator wants live tunability so they can dial in their
    // preferred ballistic).  Distinct from peakHoldMs above: that one
    // controls the small peak-cap indicator's hang/decay; THIS one
    // controls the MAIN needle / fill-bar hold via the sliding-window
    // MAX detector's window length.  Live-apply (no restart).
    // Persisted.  Range 100-10000 ms.  Default 3000 ms.
    //
    // Honest scope note: the HARDWARE attack characteristic of the HL2
    // forward-power sensing chain (directional coupler analog
    // integrator + gateware ADC sample rate) is what it is — no
    // software fix can shorten the rise time from key-down to peak.
    // Operator-observed "slow to react" is partly the HW ramp curve
    // (the needle visibly sweeps UP through ADC samples as they
    // climb toward the peak); the MAX detector itself is instant-
    // attack at the software layer.  This knob only changes how LONG
    // the captured peak holds before decaying — not how fast it
    // climbs to peak in the first place.
    Q_PROPERTY(int pwrPeakHoldMs READ pwrPeakHoldMs WRITE setPwrPeakHoldMs
               NOTIFY pwrPeakHoldMsChanged)
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
    // TX secondary digital readout (task #36).  When a TX source (PWR
    // / SWR / ALC / etc.) is the primary, the operator can pick a
    // SECOND source to render as a small digital readout under the
    // main needle (reusing the existing snrText slot the renderers
    // already display — RX uses it for SNR, TX uses it for the chosen
    // secondary).  -1 = none / hide; otherwise a Source enum value.
    // Hidden automatically when the chosen secondary equals the
    // current primary (showing "PWR 4.2 W" under a PWR meter would
    // be a tautology — Settings prevents the selection but the model
    // gates defensively).
    Q_PROPERTY(int txSecondary READ txSecondary WRITE setTxSecondary
               NOTIFY txSecondaryChanged)
    // Second TX digital readout (task #37) — brings TX into RX-parity 3-line
    // layout (primary big number / secondary #1 / secondary #2 == RX's
    // primary / dBm / SNR).  Renders via the new secondary2Text property
    // that HorizonArc / PlasmaBar both display under the existing snrText
    // line.  -1 = none / hide; otherwise a Source enum value.  Hidden when
    // it equals the current primary OR equals the first secondary (no
    // duplicate lines).
    Q_PROPERTY(int txSecondary2 READ txSecondary2 WRITE setTxSecondary2
               NOTIFY txSecondary2Changed)
    Q_PROPERTY(QString secondary2Text READ secondary2Text NOTIFY updated)
    // Ladder rows (task #38 — Multi-source view).  Populated each tick
    // with per-source state for the Vertical Ladder renderer.  Each
    // entry is a QVariantMap with keys:
    //   label    : "PWR" / "SWR" / "PA" / ...
    //   value    : "4.2 W" / "1.4:1" / "1.76 A" / ...
    //   level    : 0..1 normalized fill (palette gradient keyed off
    //              this source's own danger point)
    //   danger   : 0..1 position of the red-zone threshold for this row
    // Computed in MOX-aware fashion: TX = primary TX sources stacked;
    // RX = degraded 3-row view (S-meter / noise floor / SNR).  Only
    // populated when the active style is Ladder — saves work otherwise.
    Q_PROPERTY(QVariantList ladderRows READ ladderRows NOTIFY updated)
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
    int  pwrPeakHoldMs() const { return pwrPeakHoldMs_; }
    void setPwrPeakHoldMs(int ms);
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
    int  txSecondary() const { return txSecondary_; }
    void setTxSecondary(int s);
    int  txSecondary2() const { return txSecondary2_; }
    void setTxSecondary2(int s);
    QString secondary2Text() const { return secondary2Text_; }
    QVariantList ladderRows() const { return ladderRows_; }

    // Tick marks for the scale: list of { pos: 0..1, label: "9"/"+20", major: bool }.
    Q_INVOKABLE QVariantList tickMarks() const;

signals:
    void updated();
    void styleChanged();
    void calChanged();
    void peakHoldChanged();
    void pwrPeakHoldMsChanged();
    void maxPeakCfgChanged();
    void sourceChanged();
    void rxSourceChanged();
    void txSourceChanged();
    void pwrCalChanged();
    void txSecondaryChanged();
    void txSecondary2Changed();

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
    // Format a small "PWR 4.2 W" / "SWR 1.3:1" / "PA 1.8 A" / etc.
    // text snapshot for the given source, suitable for the secondary
    // digital readout under a TX primary.  Reads raw values from
    // stream_ without mutating any model state — safe to call from
    // any compute fn after the primary has finished its tick.
    QString formatSecondaryText(int src) const;
    // Build the per-row Ladder data based on the current MOX state.
    // Called from tick() when the active style is Ladder.  Reads raw
    // values via stream_ getters and the formatSecondaryText helper —
    // no per-row state retained (peak-hold etc. is a future polish).
    void   buildLadderRows();
    // Compute normalized level + danger threshold for a given Source
    // value.  Used by buildLadderRows().  Returns {level, danger}
    // pairs in 0..1 range so the QML renderer can paint the bar +
    // zone coloring uniformly across all rows.
    void   ladderRowFor(int src, double *level, double *danger) const;
    double normForDbm(double dbm) const;
    void   updateScale();              // pick HF/VHF endpoints from the VFO freq
    QString sLabel(double dbm) const;  // standard HF dBm→S-unit table
    // PWR cal: operator's reference anchor (10m / dummy / full TUN
    // reading on a known watt-meter) maps the provisional fwd_power ADC
    // -> watts curve to a true scalar.  Single global cal for v0.2.x
    // (per-band 3-point cal is a polish item).  Persisted under
    // meter/pwrCalScale (default 1.0 = use the provisional formula
    // as-is) + meter/pwrRatedMaxW (default 5.0 W = HL2+ on-board PA).
    double pwrCalScale_  = 1.0;     // applied to fwdPowerW() output
    double pwrRatedMaxW_ = 5.0;     // danger-zone (red) starts here
    double pwrScaleMaxW_ = 10.0;    // full-scale watts (== 2 * rated max)
    int    txSecondary_  = -1;      // -1 = hide; else Source enum value
    int    txSecondary2_ = -1;      // -1 = hide; else Source enum value
    QString secondary2Text_;        // populated by computePwr/computeSwr
    QVariantList ladderRows_;       // Multi-source rows for Ladder style
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
    // S9 = -73 dBm below 30 MHz, -93 dBm above — standard HF/VHF split).
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
    double dispDbm_ = -140.0;  // smoothed dBm for the readout (S-meter)

    // PWR meter sliding-window MAX state (operator bench 2026-05-31
    // identified the meter ballistic bug: brief whistles read low on
    // the needle because Lyra used IIR-smoothing while the verified
    // reference uses a sliding-window MAX detector — sustained tone
    // converges either way but brief peaks are averaged-out by IIR
    // and properly held by MAX).  Ring buffer of the last
    // kPwrWindowSamples ticks of measured watts; the needle reads
    // max() over the ring.  Zero-init = needle starts at 0 W and
    // rises as samples accumulate (correct first-TX behaviour).
    //
    // Window length tuning (operator bench follow-up 2026-05-31 PM):
    // initial fix used 10 samples × 50 ms = 500 ms (verified-
    // reference default).  Operator-observed peak amplitudes were
    // correct (4.6 W Lyra vs 4.3 W Palstar) but the visual hold
    // "felt instant" — a digital bar holding the same pixel value
    // for 500 ms reads as a flash because there's no visible decay
    // motion like an analog needle.  Bumped to 60 samples × 50 ms =
    // 3000 ms = 3 sec hold, matching typical Bird-Palstar PEAK
    // ballistic where a peak parks for several seconds so the
    // operator can read it at leisure.  Trade-off: rapid-fire CW
    // dits or fast voice peaks will show the highest single peak
    // for up to 3 sec — operationally normal for a peak-power
    // meter, less useful for rapidly-changing power.  If operator
    // prefers tighter hold (closer to verified-reference 500 ms)
    // OR wants live tuning, a follow-up adds a Settings → Meter
    // "PWR Peak Hold" spin box mapped to this constant.
    // Fixed-max buffer sized for the worst-case hold time (10 sec at
    // 50 ms tick = 200 slots).  Operator setter (setPwrPeakHoldMs)
    // updates pwrWinSamples_ to the CURRENT active count; only the
    // first pwrWinSamples_ entries are scanned/written.  Avoids
    // dynamic alloc on hold-time changes; cost is 200×8 B = 1.6 KB
    // member overhead, fine.
    static constexpr int kPwrWindowSamplesMax = 200;   // 10 sec at 50ms
    double pwrWinHist_[kPwrWindowSamplesMax] = {};     // zero-initialized
    int    pwrWinIdx_ = 0;
    int    pwrWinSamples_ = 60;   // active count, derived from
                                  // pwrPeakHoldMs_ / kTickMs.  Default
                                  // 60 = 3 sec; updated by the setter.
    int    pwrPeakHoldMs_ = 3000;
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
