// Lyra — panadapter spectrum display (Step 5, scene-graph renderer).
//
// A QQuickItem rendered on the GPU via the Qt Quick scene graph (RHI /
// Vulkan).  Pulls the WDSP analyzer's display-width dB array from
// WdspEngine and draws a smooth "liquid ribbon" spectrum on a glassy
// background — built to scale to 60→144 fps (the look the operator
// wants, done right the first time rather than a painter we'd redo).
//
// Rendering model (no custom shaders in v1 — QSGVertexColorMaterial
// does gradients via per-vertex RGBA):
//   * Glass background  : a gradient quad.
//   * Fluid ribbon+fill : ONE gradient triangle-strip whose TOP edge
//     rides a Catmull-Rom spline through the data (bright trace colour)
//     and whose BOTTOM edge sits at the baseline (transparent) — that
//     single strip is the smooth line AND the liquid fill body.
//   * Accuracy: 2048 analyzer points are reduced to screen columns by
//     PEAK-per-column (never drops a narrow CW/FT8 signal), then the
//     spline only smooths the connection — it passes through the peaks.
//
// Frame pump: window afterAnimating (vsync-locked) with an fps throttle
// — fetch the latest spectrum + schedule a repaint.  The throttle is
// the operator frame-rate control (added later).
//
// Behaviour (click-to-tune, marker, passband, snap, zoom, notch) is
// ported from the Python Lyra spectrum widget in later sub-steps.

#pragma once

#include <QColor>
#include <QQuickItem>

#include <vector>

class QTimer;

namespace lyra::dsp { class WdspEngine; }

namespace lyra::ui {

class Panadapter : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *engine READ engineObj WRITE setEngineObj
               NOTIFY engineChanged)
    // dB display window (vertical scale).  Operator-tunable later.
    Q_PROPERTY(double dbMin READ dbMin WRITE setDbMin NOTIFY rangeChanged)
    Q_PROPERTY(double dbMax READ dbMax WRITE setDbMax NOTIFY rangeChanged)
    // Target display fps (throttle of the vsync pump).  0 = every frame.
    Q_PROPERTY(int targetFps READ targetFps WRITE setTargetFps
               NOTIFY targetFpsChanged)
    // Temporal smoothing 0..10 (display-only EWMA across frames, like
    // old Lyra).  0 = off / raw FFT.  Higher = smoother + slower
    // response (alpha = 1 - strength/11).  Smooths visual flicker over
    // TIME (not across frequency — that was the rejected box-blur);
    // higher values trade in a little response lag.
    Q_PROPERTY(int smoothing READ smoothing WRITE setSmoothing
               NOTIFY smoothingChanged)
    // Gridline brightness 0..100.  0 = off, 100 = bright.
    Q_PROPERTY(int gridLevel READ gridLevel WRITE setGridLevel
               NOTIFY gridLevelChanged)
    // Glass sheen 0..100: a subtle diagonal light reflection over the
    // glass panel (drawn under the grid/trace).  0 = off.
    Q_PROPERTY(int glassSheen READ glassSheen WRITE setGlassSheen
               NOTIFY glassSheenChanged)
    // Trace colour MODE: 0 = Solid (one picked colour), 1 = By strength
    // (colour changes with signal level via the chosen palette LUT —
    // the intensity gradient old Lyra couldn't pull off).
    Q_PROPERTY(int traceMode READ traceMode WRITE setTraceMode
               NOTIFY traceModeChanged)
    // Solid-mode trace colour (default light cyan, like old Lyra).
    Q_PROPERTY(QColor traceColor READ traceColor WRITE setTraceColor
               NOTIFY traceColorChanged)
    // By-strength-mode palette index (lyra::palettes order).  An index
    // past the preset palettes selects the operator's CUSTOM strength
    // colour (strengthColor) — a dark->hue->bright ramp, like Amber.
    Q_PROPERTY(int palette READ palette WRITE setPalette
               NOTIFY paletteChanged)
    // Base colour for the custom by-strength ramp.
    Q_PROPERTY(QColor strengthColor READ strengthColor WRITE setStrengthColor
               NOTIFY strengthColorChanged)
    // Fill on/off.  Off = clean bright line only (ExpertSDR3 look);
    // on = translucent gradient fill beneath the line (deskHPSDR look).
    Q_PROPERTY(bool fillEnabled READ fillEnabled WRITE setFillEnabled
               NOTIFY fillEnabledChanged)
    // Fill colour, used in SOLID trace mode so the fill can be a DIFFERENT
    // colour than the trace line (old-Lyra parity).  In by-strength mode
    // the fill stays intensity-coloured (the heat-map look) and ignores
    // this.
    Q_PROPERTY(QColor fillColor READ fillColor WRITE setFillColor
               NOTIFY fillColorChanged)

public:
    explicit Panadapter(QQuickItem *parent = nullptr);

    QObject *engineObj() const;
    void     setEngineObj(QObject *o);

    double dbMin() const { return dbMin_; }
    void   setDbMin(double v);
    double dbMax() const { return dbMax_; }
    void   setDbMax(double v);

    int  targetFps() const { return targetFps_; }
    void setTargetFps(int v);

    int  smoothing() const { return smoothing_; }
    void setSmoothing(int v);

    int  gridLevel() const { return gridLevel_; }
    void setGridLevel(int v);

    int  glassSheen() const { return glassSheen_; }
    void setGlassSheen(int v);

    int  traceMode() const { return traceMode_; }
    void setTraceMode(int v);

    QColor traceColor() const { return traceColor_; }
    void   setTraceColor(const QColor &c);

    int  palette() const { return paletteIndex_; }
    void setPalette(int v);

    QColor strengthColor() const { return strengthColor_; }
    void   setStrengthColor(const QColor &c);

    bool fillEnabled() const { return fillEnabled_; }
    void setFillEnabled(bool v);

    QColor fillColor() const { return fillColor_; }
    void   setFillColor(const QColor &c);

signals:
    void engineChanged();
    void rangeChanged();
    void targetFpsChanged();
    void smoothingChanged();
    void gridLevelChanged();
    void glassSheenChanged();
    void traceModeChanged();
    void traceColorChanged();
    void paletteChanged();
    void strengthColorChanged();
    void fillEnabledChanged();
    void fillColorChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;
    void itemChange(ItemChange change,
                    const ItemChangeData &value) override;

private slots:
    void onFrame();   // timer-paced: fetch spectrum + schedule repaint

private:
    lyra::dsp::WdspEngine *engine_ = nullptr;
    std::vector<float>     pix_;        // dB pixels (post-EWMA, what renders)
    std::vector<float>     rawPix_;     // latest raw frame from the analyzer
    bool                   smoothInit_ = false;  // pix_ seeded yet?
    double                 dbMin_ = -130.0;
    double                 dbMax_ = -20.0;
    int                    targetFps_ = 60;
    int                    smoothing_ = 0;     // 0..100 spatial smoothing (off = Lyra-style raw trace)
    int                    gridLevel_ = 35;    // 0..100 gridline brightness
    int                    glassSheen_ = 20;   // 0..100 glass reflection
    int                    traceMode_ = 0;     // 0 solid, 1 by-strength
    QColor                 traceColor_ = QColor(0x5e, 0xc8, 0xff);  // solid trace colour
    int                    paletteIndex_ = 0;  // by-strength palette
    QColor                 strengthColor_ = QColor(0xff, 0x9b, 0x30);  // custom ramp base
    bool                   fillEnabled_ = true; // fill beneath the line
    QColor                 fillColor_ = QColor(0x5e, 0xc8, 0xff);  // solid-mode fill
    QTimer                *frameTimer_ = nullptr;  // fixed-cadence repaint pump

    // Per-frame scratch (reused to avoid per-frame allocation): the
    // peak-per-column curve, then the spatially-smoothed curve.
    std::vector<float>     colDb_;
    std::vector<float>     smoothDb_;
};

} // namespace lyra::ui
