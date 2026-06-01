// Lyra — shared UI preferences object.
//
// A single C++ QObject that owns operator-tunable display settings and
// persists them to QSettings.  Exposed to QML as the `Prefs` context
// property AND read/written by the QtWidgets Settings dialog — so the
// dialog and the live QML panadapter stay in sync through one source
// of truth (no reaching into QML items from C++).  All native C++;
// cross-platform.
//
// Panadapter visual options (palette, fill mode, glow, etc.) land here
// as they're built, so the Settings → Visuals tab drives them.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace lyra::ui {

class Prefs : public QObject {
    Q_OBJECT
    Q_PROPERTY(int gridLevel READ gridLevel WRITE setGridLevel
               NOTIFY gridLevelChanged)
    Q_PROPERTY(int targetFps READ targetFps WRITE setTargetFps
               NOTIFY targetFpsChanged)
    Q_PROPERTY(double dbMin READ dbMin WRITE setDbMin NOTIFY dbMinChanged)
    Q_PROPERTY(double dbMax READ dbMax WRITE setDbMax NOTIFY dbMaxChanged)
    // Auto-fit the panadapter dB range (ignores dbMin/dbMax).
    Q_PROPERTY(bool dbAuto READ dbAuto WRITE setDbAuto NOTIFY dbAutoChanged)
    Q_PROPERTY(int traceMode READ traceMode WRITE setTraceMode
               NOTIFY traceModeChanged)
    Q_PROPERTY(QString traceColor READ traceColor WRITE setTraceColor
               NOTIFY traceColorChanged)
    Q_PROPERTY(int palette READ palette WRITE setPalette NOTIFY paletteChanged)
    Q_PROPERTY(QString strengthColor READ strengthColor WRITE setStrengthColor
               NOTIFY strengthColorChanged)
    Q_PROPERTY(bool fillEnabled READ fillEnabled WRITE setFillEnabled
               NOTIFY fillEnabledChanged)
    Q_PROPERTY(QString fillColor READ fillColor WRITE setFillColor
               NOTIFY fillColorChanged)
    Q_PROPERTY(int smoothing READ smoothing WRITE setSmoothing
               NOTIFY smoothingChanged)
    Q_PROPERTY(int glow READ glow WRITE setGlow NOTIFY glowChanged)
    Q_PROPERTY(int glassSheen READ glassSheen WRITE setGlassSheen
               NOTIFY glassSheenChanged)
    // Peak-hold markers over the trace (old-Lyra parity).  peakHoldSecs
    // encodes the mode: 0 = Off, -1 = Hold (infinite), -2 = Live,
    // >0 = hold-then-decay seconds.  peakStyle: 0 line / 1 dots /
    // 2 triangles.  peakShowDb toggles numeric dB labels at the
    // strongest in-view peaks.
    Q_PROPERTY(bool peakEnabled READ peakEnabled WRITE setPeakEnabled
               NOTIFY peakEnabledChanged)
    Q_PROPERTY(double peakHoldSecs READ peakHoldSecs WRITE setPeakHoldSecs
               NOTIFY peakHoldSecsChanged)
    Q_PROPERTY(double peakDecayDbps READ peakDecayDbps WRITE setPeakDecayDbps
               NOTIFY peakDecayDbpsChanged)
    Q_PROPERTY(int peakStyle READ peakStyle WRITE setPeakStyle
               NOTIFY peakStyleChanged)
    Q_PROPERTY(QString peakColor READ peakColor WRITE setPeakColor
               NOTIFY peakColorChanged)
    Q_PROPERTY(bool peakShowDb READ peakShowDb WRITE setPeakShowDb
               NOTIFY peakShowDbChanged)
    // Noise-floor reference line on the panadapter (old-Lyra parity):
    // a dashed line at the rolling ~20th-percentile floor + an
    // "NF -NN dBFS" label.  On/off + colour are operator-tunable.
    Q_PROPERTY(bool noiseFloorEnabled READ noiseFloorEnabled
               WRITE setNoiseFloorEnabled NOTIFY noiseFloorEnabledChanged)
    Q_PROPERTY(QString noiseFloorColor READ noiseFloorColor
               WRITE setNoiseFloorColor NOTIFY noiseFloorColorChanged)
    Q_PROPERTY(bool watermark READ watermark WRITE setWatermark
               NOTIFY watermarkChanged)
    Q_PROPERTY(bool meteors READ meteors WRITE setMeteors NOTIFY meteorsChanged)
    Q_PROPERTY(int meteorGap READ meteorGap WRITE setMeteorGap
               NOTIFY meteorGapChanged)
    Q_PROPERTY(int meteorGold READ meteorGold WRITE setMeteorGold
               NOTIFY meteorGoldChanged)
    // Waterfall has its OWN palette (independent of the by-strength
    // trace palette) + its own custom-ramp base colour, plus a scroll
    // speed (rows per second).
    Q_PROPERTY(int waterfallPalette READ waterfallPalette
               WRITE setWaterfallPalette NOTIFY waterfallPaletteChanged)
    Q_PROPERTY(QString waterfallColor READ waterfallColor
               WRITE setWaterfallColor NOTIFY waterfallColorChanged)
    Q_PROPERTY(int waterfallSpeed READ waterfallSpeed
               WRITE setWaterfallSpeed NOTIFY waterfallSpeedChanged)
    // Waterfall heat-map dB range — INDEPENDENT of the panadapter's
    // dB range (dbMin/dbMax), so the waterfall contrast can be tuned on
    // its own to make detail pop (old-Lyra parity).
    Q_PROPERTY(double waterfallDbMin READ waterfallDbMin
               WRITE setWaterfallDbMin NOTIFY waterfallDbMinChanged)
    Q_PROPERTY(double waterfallDbMax READ waterfallDbMax
               WRITE setWaterfallDbMax NOTIFY waterfallDbMaxChanged)
    // Auto-fit the waterfall dB range (ignores waterfallDbMin/Max).
    Q_PROPERTY(bool waterfallDbAuto READ waterfallDbAuto
               WRITE setWaterfallDbAuto NOTIFY waterfallDbAutoChanged)
    // Floating frequency readout that follows the cursor over the
    // panadapter (on by default; operator-toggleable).
    Q_PROPERTY(bool cursorReadout READ cursorReadout WRITE setCursorReadout
               NOTIFY cursorReadoutChanged)
    // Panadapter zoom: 1.0 = full IQ span, higher magnifies the centre.
    // Drives WdspEngine.zoom (display-side crop, old-Lyra method).
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    // Demod mode (USB/LSB/CWU/…) + RX filter bandwidth (Hz) for the
    // CURRENT mode.  Bandwidth is remembered per-mode (old-Lyra style):
    // switching mode recalls that mode's last bandwidth.  Drives
    // WdspEngine.mode / .bandwidth.
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(int rxBandwidth READ rxBandwidth WRITE setRxBandwidth
               NOTIFY rxBandwidthChanged)
    // TX Component 8c — TX filter bandwidth (Hz) for the CURRENT mode.
    // Per-mode just like rxBandwidth.  For SSB the value is the high
    // edge (low fixed at 200 Hz via TxChannel::open() default until a
    // separate Low spinbox lands per design doc §9.2).  Drives the
    // TX channel via HL2Stream::setTxBwHz.  See bwLocked for the
    // mirror-to-RX semantics.
    Q_PROPERTY(int txBandwidth READ txBandwidth WRITE setTxBandwidth
               NOTIFY txBandwidthChanged)
    // TX Component 8c — RX↔TX bandwidth mirror.  When ON, setting
    // either side updates the other (both directions, single-step,
    // no recursion).  Toggling ON pulls RX into TX (matches old-Lyra
    // direction).  Per-mode storage is unchanged; the mirror only
    // touches the CURRENT mode's pair.
    Q_PROPERTY(bool bwLocked READ bwLocked WRITE setBwLocked
               NOTIFY bwLockedChanged)
    // Task #53 — shared RX+TX filter low edge (Hz, single global
    // value).  Replaces the previously-hardcoded 0 (RX SSB low)
    // and 200 (TX SSB low).  Operator-tunable for ESSB-profile
    // low-end body (50-70 Hz) vs narrow-comms mains rejection
    // (100-200 Hz).  Interim — when TX Profile Manager (#49) lands,
    // each named profile carries its own (rx_lo, rx_hi, tx_lo,
    // tx_hi) tuple that overrides this default on profile load.
    Q_PROPERTY(int filterLow READ filterLow WRITE setFilterLow
               NOTIFY filterLowChanged)
    // IQ sample rate (Hz): 96000 / 192000 / 384000.  Drives the HL2 wire
    // speed bits + the WDSP channel rate (panadapter span follows).
    Q_PROPERTY(int sampleRate READ sampleRate WRITE setSampleRate
               NOTIFY sampleRateChanged)
    // Waterfall collapsed (old-Lyra triangle toggle) — when true the
    // waterfall pane is hidden and the panadapter takes the full height.
    Q_PROPERTY(bool waterfallCollapsed READ waterfallCollapsed
               WRITE setWaterfallCollapsed NOTIFY waterfallCollapsedChanged)
    // Opaque serialized state of the panadapter/waterfall SplitView
    // (the divider position).  QMainWindow.saveState() does NOT cover
    // splitters inside a dock's QML content, so it's persisted here as
    // the QML SplitView.saveState()/restoreState() blob.  QVariant so
    // the value round-trips through QSettings byte-exact.
    Q_PROPERTY(QVariant panadapterSplit READ panadapterSplit
               WRITE setPanadapterSplit NOTIFY panadapterSplitChanged)
    // Operator / station identity (Settings → Hardware).  Callsign feeds
    // TCI/logging; the location (grid, or manual lat/lon override) feeds
    // the weather sources + solar panel.  Effective lat/lon = grid when
    // valid, else the manual override.
    Q_PROPERTY(QString callsign READ callsign WRITE setCallsign
               NOTIFY callsignChanged)
    Q_PROPERTY(QString gridSquare READ gridSquare WRITE setGridSquare
               NOTIFY gridSquareChanged)
    // Amateur band-plan region for panadapter overlays (sub-band
    // segments, landmarks, out-of-band advisories).  Values:
    // US / IARU_R1 / IARU_R3 / NONE.
    Q_PROPERTY(QString bandPlanRegion READ bandPlanRegion
               WRITE setBandPlanRegion NOTIFY bandPlanRegionChanged)
    // Band-plan panadapter-overlay layer toggles (Settings → Hardware →
    // Band plan).  All default ON; the master Region=NONE hides everything.
    Q_PROPERTY(bool bandPlanSegments READ bandPlanSegments
               WRITE setBandPlanSegments NOTIFY bandPlanSegmentsChanged)
    Q_PROPERTY(bool bandPlanLandmarks READ bandPlanLandmarks
               WRITE setBandPlanLandmarks NOTIFY bandPlanLandmarksChanged)
    Q_PROPERTY(bool bandPlanBeacons READ bandPlanBeacons
               WRITE setBandPlanBeacons NOTIFY bandPlanBeaconsChanged)
    Q_PROPERTY(bool bandPlanEdges READ bandPlanEdges
               WRITE setBandPlanEdges NOTIFY bandPlanEdgesChanged)
    // Show the 11m / CB band row on the Band panel (Settings → Hardware).
    Q_PROPERTY(bool cbBandEnabled READ cbBandEnabled
               WRITE setCbBandEnabled NOTIFY cbBandEnabledChanged)
    // Panadapter mouse-wheel "Panafall" scroll step (Hz) — distinct from
    // the fine VFO step on the Tuning panel; this skims across a band.
    Q_PROPERTY(int panScrollStepHz READ panScrollStepHz
               WRITE setPanScrollStepHz NOTIFY panScrollStepHzChanged)
    // When true, panadapter freq-set gestures (click/drag/wheel) round the
    // resulting (operator-facing) frequency to the nearest 100 Hz.
    Q_PROPERTY(bool panRound100 READ panRound100
               WRITE setPanRound100 NOTIFY panRound100Changed)
    // Verbose diagnostic logging (Settings → Hardware → Diagnostics).
    // OFF (default) keeps only warnings/errors in the log; ON also
    // captures Debug/Info for when we need the full picture.
    Q_PROPERTY(bool debugLogging READ debugLogging WRITE setDebugLogging
               NOTIFY debugLoggingChanged)
    // Task #36 — Hardware PTT input (foot switch / hand mic / mic button)
    // forwarder.  Default OFF — per the project-memory §10 Q#1 finding,
    // N8SDR's HL2+/AK4951 carries a non-zero EP6 C0 bit 0 (ptt_in) at
    // RX rest, so an always-on forwarder mis-reads it as a foot-switch
    // press → spurious MOX → phantom-TX surge.  Operator opts in via
    // Settings → TX → Advanced AFTER bench-verifying that ptt_in is a
    // clean 0 at RX rest on their specific unit (or after confirming
    // they have a real foot-switch / mic-button wired to the HL2 PTT
    // input).  Persisted: tx/hw_ptt_enabled.
    Q_PROPERTY(bool hwPttEnabled READ hwPttEnabled WRITE setHwPttEnabled
               NOTIFY hwPttEnabledChanged)

    // Task #33 — TX mic source.  Operator-selected via Settings → TX
    // → Mic Source.  Token strings match the TCI v2 TRX source-token
    // convention (TCI spec §3.3) so a TCI client that sends
    // `trx:0,true,tci` selects the TCI source automatically (commit
    // 3's TRX handler auto-flips this).  Persisted: tx/mic_source
    // (default "mic1" = current v0.2.0..v0.2.2 codec-mic path).
    //
    // Recognised tokens:
    //   "mic1"    — HL2/HL2+ codec mic input (Hl2Ep6MicSource)
    //   "tci"     — inbound TCI v2 TX_AUDIO_STREAM (TciMicSource)
    //   "mic2"    — HL2+ codec Line In (HL2 I²C2 — future v0.2.x)
    //   "micpc"   — host PC audio capture (future v0.2.x VAC1)
    //   "micpc2"  — second host PC capture device (future VAC2)
    //
    // Unknown tokens fall back to "mic1" (safety: never end up
    // routing to an inactive source).
    Q_PROPERTY(QString micSource READ micSource WRITE setMicSource
               NOTIFY micSourceChanged)

public:
    explicit Prefs(QObject *parent = nullptr);

    // Palette display names (presets + "Custom color…") for QML pickers
    // — index-aligned with the palette/waterfallPalette int values.
    Q_INVOKABLE QStringList paletteNames() const;

    // Fire-and-forget "clear the peak-hold buffer" request from the
    // Display panel's Clear button — the panadapter (a different dock)
    // listens on peakClearRequested and calls clearPeaks().
    Q_INVOKABLE void requestClearPeaks() { emit peakClearRequested(); }

    int  gridLevel() const { return gridLevel_; }
    void setGridLevel(int v);
    int  targetFps() const { return targetFps_; }
    void setTargetFps(int v);
    double dbMin() const { return dbMin_; }
    void   setDbMin(double v);
    double dbMax() const { return dbMax_; }
    void   setDbMax(double v);
    bool dbAuto() const { return dbAuto_; }
    void setDbAuto(bool v);
    int  traceMode() const { return traceMode_; }
    void setTraceMode(int v);
    QString traceColor() const { return traceColor_; }
    void    setTraceColor(const QString &hex);
    int  palette() const { return palette_; }
    void setPalette(int v);
    QString strengthColor() const { return strengthColor_; }
    void    setStrengthColor(const QString &hex);
    bool fillEnabled() const { return fillEnabled_; }
    void setFillEnabled(bool v);
    QString fillColor() const { return fillColor_; }
    void    setFillColor(const QString &hex);
    int  smoothing() const { return smoothing_; }
    void setSmoothing(int v);
    int  glow() const { return glow_; }
    void setGlow(int v);
    int  glassSheen() const { return glassSheen_; }
    void setGlassSheen(int v);
    bool peakEnabled() const { return peakEnabled_; }
    void setPeakEnabled(bool v);
    double peakHoldSecs() const { return peakHoldSecs_; }
    void   setPeakHoldSecs(double v);
    double peakDecayDbps() const { return peakDecayDbps_; }
    void   setPeakDecayDbps(double v);
    int  peakStyle() const { return peakStyle_; }
    void setPeakStyle(int v);
    QString peakColor() const { return peakColor_; }
    void    setPeakColor(const QString &hex);
    bool peakShowDb() const { return peakShowDb_; }
    void setPeakShowDb(bool v);
    bool noiseFloorEnabled() const { return noiseFloorEnabled_; }
    void setNoiseFloorEnabled(bool v);
    QString noiseFloorColor() const { return noiseFloorColor_; }
    void    setNoiseFloorColor(const QString &hex);
    bool watermark() const { return watermark_; }
    void setWatermark(bool v);
    bool meteors() const { return meteors_; }
    void setMeteors(bool v);
    int  meteorGap() const { return meteorGap_; }
    void setMeteorGap(int v);
    int  meteorGold() const { return meteorGold_; }
    void setMeteorGold(int v);
    int  waterfallPalette() const { return waterfallPalette_; }
    void setWaterfallPalette(int v);
    QString waterfallColor() const { return waterfallColor_; }
    void    setWaterfallColor(const QString &hex);
    int  waterfallSpeed() const { return waterfallSpeed_; }
    void setWaterfallSpeed(int v);
    double waterfallDbMin() const { return waterfallDbMin_; }
    void   setWaterfallDbMin(double v);
    double waterfallDbMax() const { return waterfallDbMax_; }
    void   setWaterfallDbMax(double v);
    bool waterfallDbAuto() const { return waterfallDbAuto_; }
    void setWaterfallDbAuto(bool v);
    QVariant panadapterSplit() const { return panadapterSplit_; }
    void     setPanadapterSplit(const QVariant &v);
    bool cursorReadout() const { return cursorReadout_; }
    void setCursorReadout(bool v);
    double zoom() const { return zoom_; }
    void   setZoom(double v);
    QString mode() const { return mode_; }
    void    setMode(const QString &m);
    int  rxBandwidth() const;            // bandwidth for the current mode
    void setRxBandwidth(int hz);
    // TX Component 8c — current-mode TX bandwidth, mirroring rxBandwidth.
    int  txBandwidth() const;
    void setTxBandwidth(int hz);
    bool bwLocked() const { return bwLocked_; }
    void setBwLocked(bool v);
    // Task #53 — shared RX+TX filter low edge.
    int  filterLow() const { return filterLow_; }
    void setFilterLow(int hz);
    int  sampleRate() const { return sampleRate_; }
    void setSampleRate(int hz);
    bool waterfallCollapsed() const { return waterfallCollapsed_; }
    void setWaterfallCollapsed(bool v);
    // --- operator / station identity ---
    QString callsign() const { return callsign_; }
    void    setCallsign(const QString &c);
    QString gridSquare() const { return gridSquare_; }
    void    setGridSquare(const QString &g);   // normalized; "" if invalid
    double  manualLat() const { return manualLat_; }   // NaN = unset
    double  manualLon() const { return manualLon_; }
    // Set/clear the manual lat/lon override (pass NaN,NaN to clear).
    Q_INVOKABLE void setManualLatLon(double lat, double lon);
    // Effective operator location: grid (if valid) else manual override.
    // Returns false (and leaves *lat/*lon untouched) when neither is set.
    bool operatorLocation(double *lat, double *lon) const;
    QString bandPlanRegion() const { return bandPlanRegion_; }
    void    setBandPlanRegion(const QString &r);
    bool bandPlanSegments() const { return bandPlanSegments_; }
    void setBandPlanSegments(bool v);
    bool bandPlanLandmarks() const { return bandPlanLandmarks_; }
    void setBandPlanLandmarks(bool v);
    bool bandPlanBeacons() const { return bandPlanBeacons_; }
    void setBandPlanBeacons(bool v);
    bool bandPlanEdges() const { return bandPlanEdges_; }
    void setBandPlanEdges(bool v);
    bool cbBandEnabled() const { return cbBandEnabled_; }
    void setCbBandEnabled(bool v);
    int  panScrollStepHz() const { return panScrollStepHz_; }
    void setPanScrollStepHz(int hz);
    bool panRound100() const { return panRound100_; }
    void setPanRound100(bool v);
    // Per-mode-kind band-plan segment colour (override of the defaults).
    // kind = "CW"/"DIG"/"SSB"/"FM"/"MIX"/"BC".  Always returns a colour.
    QString bandPlanColor(const QString &kind) const;
    void    setBandPlanColor(const QString &kind, const QString &hex);
    static QString defaultBandPlanColor(const QString &kind);
    bool debugLogging() const { return debugLogging_; }
    void setDebugLogging(bool on);
    // Task #36 — Hardware PTT input opt-in.  Default false (the §10 Q#1
    // safety-first posture).  Setter persists + emits.
    bool hwPttEnabled() const { return hwPttEnabled_; }
    void setHwPttEnabled(bool on);

    QString micSource() const { return micSource_; }
    void    setMicSource(const QString &token);
    // Task #33 — operator-facing token list with display labels.  Used
    // by the Settings → TX → Mic Source dropdown.  Order matches the
    // dropdown order (Mic In default first).  Disabled entries get a
    // tooltip from micSourceTooltip() below.
    static QStringList micSourceTokens();
    static QString     micSourceLabel(const QString &token);
    static bool        micSourceEnabled(const QString &token);
    static QString     micSourceTooltip(const QString &token);
    // Built-in default RX bandwidth for a mode (first run / unset).
    static int defaultBandwidthFor(const QString &mode);

signals:
    void gridLevelChanged();
    void targetFpsChanged();
    void dbMinChanged();
    void dbMaxChanged();
    void dbAutoChanged();
    void traceModeChanged();
    void traceColorChanged();
    void paletteChanged();
    void strengthColorChanged();
    void fillEnabledChanged();
    void fillColorChanged();
    void smoothingChanged();
    void glowChanged();
    void glassSheenChanged();
    void peakEnabledChanged();
    void peakHoldSecsChanged();
    void peakDecayDbpsChanged();
    void peakStyleChanged();
    void peakColorChanged();
    void peakShowDbChanged();
    void peakClearRequested();
    void noiseFloorEnabledChanged();
    void noiseFloorColorChanged();
    void watermarkChanged();
    void meteorsChanged();
    void meteorGapChanged();
    void meteorGoldChanged();
    void waterfallPaletteChanged();
    void waterfallColorChanged();
    void waterfallSpeedChanged();
    void waterfallDbMinChanged();
    void waterfallDbMaxChanged();
    void waterfallDbAutoChanged();
    void panadapterSplitChanged();
    void cursorReadoutChanged();
    void zoomChanged();
    void modeChanged();
    void rxBandwidthChanged();
    void txBandwidthChanged();
    void bwLockedChanged();
    void filterLowChanged();
    void sampleRateChanged();
    void waterfallCollapsedChanged();
    void callsignChanged();
    void gridSquareChanged();
    void locationChanged();   // effective lat/lon changed (grid or manual)
    void bandPlanRegionChanged();
    void bandPlanSegmentsChanged();
    void bandPlanLandmarksChanged();
    void bandPlanBeaconsChanged();
    void bandPlanEdgesChanged();
    void bandPlanColorsChanged();
    void cbBandEnabledChanged();
    void panScrollStepHzChanged();
    void panRound100Changed();
    void debugLoggingChanged();
    void hwPttEnabledChanged();
    void micSourceChanged();

private:
    int     gridLevel_;
    int     targetFps_;
    double  dbMin_;
    double  dbMax_;
    bool    dbAuto_;
    int     traceMode_;
    QString traceColor_;
    int     palette_;
    QString strengthColor_;
    bool    fillEnabled_;
    QString fillColor_;
    int     smoothing_;
    int     glow_;
    int     glassSheen_;
    bool    peakEnabled_;
    double  peakHoldSecs_;
    double  peakDecayDbps_;
    int     peakStyle_;
    QString peakColor_;
    bool    peakShowDb_;
    bool    noiseFloorEnabled_;
    QString noiseFloorColor_;
    bool    watermark_;
    bool    meteors_;
    int     meteorGap_;
    int     meteorGold_;
    int     waterfallPalette_;
    QString waterfallColor_;
    int     waterfallSpeed_;
    double  waterfallDbMin_;
    double  waterfallDbMax_;
    bool    waterfallDbAuto_;
    QVariant panadapterSplit_;
    bool    cursorReadout_;
    double  zoom_;
    QString mode_;
    QHash<QString, int> bwByMode_;   // per-mode RX bandwidth memory
    // TX Component 8c — per-mode TX bandwidth memory + the RX↔TX
    // lock flag.  Defaults come from defaultBandwidthFor() if the
    // operator hasn't picked a per-mode TX BW yet (fresh install).
    QHash<QString, int> txBwByMode_;
    bool                bwLocked_ = false;
    // Task #53 — shared RX+TX filter low edge.  Default 100 Hz
    // (safe mains-rejection middle ground).
    int                 filterLow_ = 100;
    int     sampleRate_;
    bool    waterfallCollapsed_;
    QString callsign_;
    QString gridSquare_;
    double  manualLat_;   // NaN = unset
    double  manualLon_;   // NaN = unset
    QString bandPlanRegion_;
    bool    bandPlanSegments_  = true;
    bool    bandPlanLandmarks_ = true;
    bool    bandPlanBeacons_   = true;
    bool    bandPlanEdges_     = true;
    QHash<QString, QString> bandPlanColors_;   // kind → override hex (sparse)
    bool    cbBandEnabled_ = false;
    int     panScrollStepHz_ = 1000;
    bool    panRound100_ = false;
    bool    debugLogging_ = false;
    // Task #36 — HW PTT opt-in.  Default false; gated forwarder in
    // HL2Stream's RX loop reads the mirrored atomic.
    bool    hwPttEnabled_ = false;
    // Task #33 — TX mic source token.  Default "mic1" matches the
    // v0.2.0..v0.2.2 ship behaviour.  Persisted: tx/mic_source.
    QString micSource_   = QStringLiteral("mic1");
};

} // namespace lyra::ui
