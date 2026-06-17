// Lyra — Combinator model (#51).  The "model" half of the Combinator
// model/view: owns a lyra::dsp::Combinator engine and exposes it to QML
// (CombinatorPanel.qml) as the `Combinator` context property.
//
// Mirrors the EqModel idiom: Q_PROPERTY globals + index-keyed Q_INVOKABLE
// per-band getters/setters (the QML view forces re-eval off paramsChanged),
// a static txProcessCb + s_txEngine atomic bridge for the wire layer
// (registered in main.cpp at Stage 3), and a GUI poll timer that republishes
// the per-band meters.  Default stage state OFF (bypassed) — the Combinator
// materially compresses, so it ships off until the operator enables it.

#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>

#include <array>
#include <atomic>

#include "dsp/Combinator.h"

class QTimer;

namespace lyra::ui {

class CombinatorModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool bypass READ bypass WRITE setBypass NOTIFY bypassChanged)
    Q_PROPERTY(double mix READ mix WRITE setMix NOTIFY paramsChanged)
    Q_PROPERTY(double attackMs READ attackMs WRITE setAttackMs NOTIFY paramsChanged)
    Q_PROPERTY(double releaseMs READ releaseMs WRITE setReleaseMs NOTIFY paramsChanged)
    Q_PROPERTY(double ratio READ ratio WRITE setRatio NOTIFY paramsChanged)
    Q_PROPERTY(double threshDb READ threshDb WRITE setThreshDb NOTIFY paramsChanged)
    Q_PROPERTY(double makeupDb READ makeupDb WRITE setMakeupDb NOTIFY paramsChanged)
    Q_PROPERTY(double xover READ xover WRITE setXover NOTIFY paramsChanged)
    Q_PROPERTY(bool sbcEnabled READ sbcEnabled WRITE setSbcEnabled NOTIFY paramsChanged)
    Q_PROPERTY(double sbcSpeed READ sbcSpeed WRITE setSbcSpeed NOTIFY paramsChanged)
    Q_PROPERTY(int selectedBand READ selectedBand WRITE setSelectedBand
                   NOTIFY selectedBandChanged)
    Q_PROPERTY(int numBands READ numBands CONSTANT)

public:
    explicit CombinatorModel(QObject *parent = nullptr);
    ~CombinatorModel() override;

    // C-callable bridge for the wire-layer TX pump (Stage 3: CMaster calls
    // this per TX block via SendpTxCombinatorProcessor).  No-op when no
    // model exists or the stage is bypassed.
    static void txProcessCb(int nSamples, double *iqPairs);

    // Live single instance (#49 profile capture/apply bridge).
    static CombinatorModel *instance() { return s_self; }

    bool   bypass()    const { return bypass_; }
    double mix()       const { return mix_; }
    double attackMs()  const { return attMs_; }
    double releaseMs() const { return relMs_; }
    double ratio()     const { return ratio_; }
    double threshDb()  const { return threshDb_; }
    double makeupDb()  const { return makeupDb_; }
    double xover()     const { return xover_; }
    bool   sbcEnabled()const { return sbcOn_; }
    double sbcSpeed()  const { return sbcSpeed_; }
    int    selectedBand() const { return selected_; }
    int    numBands()  const { return lyra::dsp::Combinator::kNumBands; }

    void setBypass(bool on);
    void setMix(double v);
    void setAttackMs(double v);
    void setReleaseMs(double v);
    void setRatio(double v);
    void setThreshDb(double v);
    void setMakeupDb(double v);
    void setXover(double v);
    void setSbcEnabled(bool on);
    void setSbcSpeed(double v);
    void setSelectedBand(int i);

    // Per-band read / edit (0=LOW .. 4=HIGH).
    Q_INVOKABLE double bandThreshDb(int i) const;
    Q_INVOKABLE double bandGainDb(int i) const;
    Q_INVOKABLE bool   bandEnabled(int i) const;
    Q_INVOKABLE void   setBandThreshDb(int i, double db);
    Q_INVOKABLE void   setBandGainDb(int i, double db);
    Q_INVOKABLE void   setBandEnabled(int i, bool on);

    // Meters (read after metersChanged) + labels / split.
    Q_INVOKABLE double bandReductionDb(int i) const;
    Q_INVOKABLE double bandPeakDb(int i) const;
    Q_INVOKABLE double bandSbcDb(int i) const;
    Q_INVOKABLE double xoverHz(int k) const;
    Q_INVOKABLE QString bandName(int i) const;

    lyra::dsp::Combinator *engine() { return &cmb_; }

    // Profile bundle (#49): serialize / restore the full combinator state
    // (globals + per-band).  Meters/selectedBand are NOT included.
    QJsonObject saveState() const;
    void        loadState(const QJsonObject &o);

signals:
    void bypassChanged();
    void paramsChanged();        // any global or per-band edit (re-eval bindings)
    void selectedBandChanged();
    void metersChanged();        // poll tick — re-read the meter getters

private slots:
    void pollMeters();

private:
    bool valid(int i) const {
        return i >= 0 && i < lyra::dsp::Combinator::kNumBands;
    }
    static constexpr int kN = lyra::dsp::Combinator::kNumBands;

    static std::atomic<lyra::dsp::Combinator *> s_txEngine;
    static CombinatorModel *s_self;

    lyra::dsp::Combinator cmb_;
    QTimer *pollTimer_ = nullptr;

    // Cached UI-side state (model is the source of truth; pushed to engine).
    bool   bypass_   = true;     // default OFF (R6)
    double mix_      = 1.0;
    double attMs_    = 11.0;
    double relMs_    = 494.0;
    double ratio_    = 3.0;
    double threshDb_ = -34.0;
    double makeupDb_ = 0.0;
    double xover_    = -10.0;
    bool   sbcOn_    = true;
    double sbcSpeed_ = 4.0;
    int    selected_ = 2;        // MID

    std::array<double, kN> bandThresh_{ {-3.5, -4.0, -1.0, -1.5, -2.0} };
    std::array<double, kN> bandGain_  { { 1.5,  1.0,  0.0,  2.5,  3.0} };
    std::array<bool,   kN> bandOn_     { {true, true, true, true, true} };
};

}  // namespace lyra::ui
