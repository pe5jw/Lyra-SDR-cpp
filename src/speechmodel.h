// Lyra — TX speech-rack model (#88).  The "model" half of the speech
// model/view: owns a lyra::dsp::SpeechProcessor and exposes it to QML
// (SpeechPanel.qml) as the `Speech` context property, plus a static
// C-callable bridge the wire-layer TX pump calls (pre-EQ).  Mirrors
// EqModel exactly (atomic engine pointer, ctor publishes / dtor clears).

#pragma once

#include <QObject>

#include <atomic>

#include "dsp/SpeechProcessor.h"

namespace lyra::ui {

class SpeechModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool gateEnabled READ gateEnabled WRITE setGateEnabled NOTIFY changed)
    Q_PROPERTY(double gateThreshDb READ gateThreshDb WRITE setGateThreshDb NOTIFY changed)
    Q_PROPERTY(double gateRangeDb READ gateRangeDb WRITE setGateRangeDb NOTIFY changed)
    Q_PROPERTY(double gateHoldMs READ gateHoldMs WRITE setGateHoldMs NOTIFY changed)
    Q_PROPERTY(bool agcEnabled READ agcEnabled WRITE setAgcEnabled NOTIFY changed)
    Q_PROPERTY(double agcTargetDb READ agcTargetDb WRITE setAgcTargetDb NOTIFY changed)
    Q_PROPERTY(double agcMaxGainDb READ agcMaxGainDb WRITE setAgcMaxGainDb NOTIFY changed)
    Q_PROPERTY(bool deessEnabled READ deessEnabled WRITE setDeessEnabled NOTIFY changed)
    Q_PROPERTY(double deessFreqHz READ deessFreqHz WRITE setDeessFreqHz NOTIFY changed)
    Q_PROPERTY(double deessThreshDb READ deessThreshDb WRITE setDeessThreshDb NOTIFY changed)
    Q_PROPERTY(double deessRangeDb READ deessRangeDb WRITE setDeessRangeDb NOTIFY changed)

public:
    explicit SpeechModel(QObject *parent = nullptr);
    ~SpeechModel() override;

    bool   gateEnabled() const   { return gateEnabled_; }
    double gateThreshDb() const  { return gateThreshDb_; }
    double gateRangeDb() const   { return gateRangeDb_; }
    double gateHoldMs() const    { return gateHoldMs_; }
    bool   agcEnabled() const   { return agcEnabled_; }
    double agcTargetDb() const  { return agcTargetDb_; }
    double agcMaxGainDb() const { return agcMaxGainDb_; }
    bool   deessEnabled() const   { return deessEnabled_; }
    double deessFreqHz() const    { return deessFreqHz_; }
    double deessThreshDb() const  { return deessThreshDb_; }
    double deessRangeDb() const   { return deessRangeDb_; }

    void setGateEnabled(bool on);
    void setGateThreshDb(double db);
    void setGateRangeDb(double db);
    void setGateHoldMs(double ms);
    void setAgcEnabled(bool on);
    void setAgcTargetDb(double db);
    void setAgcMaxGainDb(double db);
    void setDeessEnabled(bool on);
    void setDeessFreqHz(double hz);
    void setDeessThreshDb(double db);
    void setDeessRangeDb(double db);

    // Wire-layer TX pump bridge (CMaster registers via SendpTxSpeechProcessor;
    // called per TX block on the mic buffer BEFORE the EQ hook).  Routes
    // through the live engine (atomic, lock-free); no-op when no model or
    // both stages off.
    static void txProcessCb(int nSamples, double *iqPairs);

    lyra::dsp::SpeechProcessor *engine() { return &sp_; }

signals:
    void changed();

private:
    static std::atomic<lyra::dsp::SpeechProcessor *> s_txEngine;

    lyra::dsp::SpeechProcessor sp_;
    bool   gateEnabled_   = false;
    double gateThreshDb_  = -45.0;
    double gateRangeDb_   = 60.0;
    double gateHoldMs_    = 120.0;
    bool   agcEnabled_   = false;
    double agcTargetDb_  = -16.0;
    double agcMaxGainDb_ = 18.0;
    bool   deessEnabled_  = false;
    double deessFreqHz_   = 6500.0;
    double deessThreshDb_ = -24.0;
    double deessRangeDb_  = 8.0;
};

}  // namespace lyra::ui
