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
    // Opaque serialized state of the panadapter/waterfall SplitView
    // (the divider position).  QMainWindow.saveState() does NOT cover
    // splitters inside a dock's QML content, so it's persisted here as
    // the QML SplitView.saveState()/restoreState() blob.  QVariant so
    // the value round-trips through QSettings byte-exact.
    Q_PROPERTY(QVariant panadapterSplit READ panadapterSplit
               WRITE setPanadapterSplit NOTIFY panadapterSplitChanged)

public:
    explicit Prefs(QObject *parent = nullptr);

    // Palette display names (presets + "Custom color…") for QML pickers
    // — index-aligned with the palette/waterfallPalette int values.
    Q_INVOKABLE QStringList paletteNames() const;

    int  gridLevel() const { return gridLevel_; }
    void setGridLevel(int v);
    int  targetFps() const { return targetFps_; }
    void setTargetFps(int v);
    double dbMin() const { return dbMin_; }
    void   setDbMin(double v);
    double dbMax() const { return dbMax_; }
    void   setDbMax(double v);
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

signals:
    void gridLevelChanged();
    void targetFpsChanged();
    void dbMinChanged();
    void dbMaxChanged();
    void traceModeChanged();
    void traceColorChanged();
    void paletteChanged();
    void strengthColorChanged();
    void fillEnabledChanged();
    void fillColorChanged();
    void smoothingChanged();
    void glowChanged();
    void glassSheenChanged();
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

private:
    int     gridLevel_;
    int     targetFps_;
    double  dbMin_;
    double  dbMax_;
    int     traceMode_;
    QString traceColor_;
    int     palette_;
    QString strengthColor_;
    bool    fillEnabled_;
    QString fillColor_;
    int     smoothing_;
    int     glow_;
    int     glassSheen_;
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
};

} // namespace lyra::ui
