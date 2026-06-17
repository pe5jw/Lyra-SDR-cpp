// Lyra — CombinatorModel implementation.  See combinatormodel.h.

#include "combinatormodel.h"

#include <QTimer>
#include <QJsonArray>

namespace lyra::ui {

std::atomic<lyra::dsp::Combinator *> CombinatorModel::s_txEngine{nullptr};

CombinatorModel::CombinatorModel(QObject *parent) : QObject(parent) {
    cmb_.setSampleRate(48000.0);   // HL2 mic / TX rack rate

    // Push the cached N8SDR default preset into the engine (the engine
    // ctor already seeds the same values; this keeps model + engine in
    // lockstep and is the single push path the setters reuse).
    cmb_.setMix(mix_);
    cmb_.setAttackMs(attMs_);
    cmb_.setReleaseMs(relMs_);
    cmb_.setRatio(ratio_);
    cmb_.setThreshDb(threshDb_);
    cmb_.setMakeupDb(makeupDb_);
    cmb_.setXover(xover_);
    cmb_.setSbcEnabled(sbcOn_);
    cmb_.setSbcSpeed(sbcSpeed_);
    for (int b = 0; b < kN; ++b) {
        cmb_.setBandThreshDb(b, bandThresh_[b]);
        cmb_.setBandGainDb(b, bandGain_[b]);
        cmb_.setBandEnabled(b, bandOn_[b]);
    }
    cmb_.setBypass(bypass_);        // default OFF

    s_txEngine.store(&cmb_, std::memory_order_release);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(33);    // ~30 Hz meter refresh
    connect(pollTimer_, &QTimer::timeout, this, &CombinatorModel::pollMeters);
    pollTimer_->start();
}

CombinatorModel::~CombinatorModel() {
    s_txEngine.store(nullptr, std::memory_order_release);
}

void CombinatorModel::txProcessCb(int nSamples, double *iqPairs) {
    auto *e = s_txEngine.load(std::memory_order_acquire);
    if (e) e->processInterleaved(iqPairs, nSamples);
}

void CombinatorModel::setBypass(bool on) {
    if (on == bypass_) return;
    bypass_ = on;
    cmb_.setBypass(on);
    emit bypassChanged();
}
void CombinatorModel::setMix(double v) {
    if (v == mix_) return;
    mix_ = v; cmb_.setMix(v); emit paramsChanged();
}
void CombinatorModel::setAttackMs(double v) {
    if (v == attMs_) return;
    attMs_ = v; cmb_.setAttackMs(v); emit paramsChanged();
}
void CombinatorModel::setReleaseMs(double v) {
    if (v == relMs_) return;
    relMs_ = v; cmb_.setReleaseMs(v); emit paramsChanged();
}
void CombinatorModel::setRatio(double v) {
    if (v == ratio_) return;
    ratio_ = v; cmb_.setRatio(v); emit paramsChanged();
}
void CombinatorModel::setThreshDb(double v) {
    if (v == threshDb_) return;
    threshDb_ = v; cmb_.setThreshDb(v); emit paramsChanged();
}
void CombinatorModel::setMakeupDb(double v) {
    if (v == makeupDb_) return;
    makeupDb_ = v; cmb_.setMakeupDb(v); emit paramsChanged();
}
void CombinatorModel::setXover(double v) {
    if (v == xover_) return;
    xover_ = v; cmb_.setXover(v); emit paramsChanged();
}
void CombinatorModel::setSbcEnabled(bool on) {
    if (on == sbcOn_) return;
    sbcOn_ = on; cmb_.setSbcEnabled(on); emit paramsChanged();
}
void CombinatorModel::setSbcSpeed(double v) {
    if (v == sbcSpeed_) return;
    sbcSpeed_ = v; cmb_.setSbcSpeed(v); emit paramsChanged();
}
void CombinatorModel::setSelectedBand(int i) {
    if (i == selected_ || !valid(i)) return;
    selected_ = i; emit selectedBandChanged();
}

double CombinatorModel::bandThreshDb(int i) const { return valid(i) ? bandThresh_[i] : 0.0; }
double CombinatorModel::bandGainDb(int i)   const { return valid(i) ? bandGain_[i]   : 0.0; }
bool   CombinatorModel::bandEnabled(int i)  const { return valid(i) && bandOn_[i]; }

void CombinatorModel::setBandThreshDb(int i, double db) {
    if (!valid(i) || db == bandThresh_[i]) return;
    bandThresh_[i] = db; cmb_.setBandThreshDb(i, db); emit paramsChanged();
}
void CombinatorModel::setBandGainDb(int i, double db) {
    if (!valid(i) || db == bandGain_[i]) return;
    bandGain_[i] = db; cmb_.setBandGainDb(i, db); emit paramsChanged();
}
void CombinatorModel::setBandEnabled(int i, bool on) {
    if (!valid(i) || on == bandOn_[i]) return;
    bandOn_[i] = on; cmb_.setBandEnabled(i, on); emit paramsChanged();
}

double CombinatorModel::bandReductionDb(int i) const { return cmb_.bandReductionDb(i); }
double CombinatorModel::bandPeakDb(int i)      const { return cmb_.bandPeakDb(i); }
double CombinatorModel::bandSbcDb(int i)       const { return cmb_.bandSbcDb(i); }
double CombinatorModel::xoverHz(int k)         const { return cmb_.xoverHz(k); }

QString CombinatorModel::bandName(int i) const {
    switch (i) {
    case 0: return QStringLiteral("LOW");
    case 1: return QStringLiteral("LO-MID");
    case 2: return QStringLiteral("MID");
    case 3: return QStringLiteral("HI-MID");
    case 4: return QStringLiteral("HIGH");
    }
    return QString();
}

QJsonObject CombinatorModel::saveState() const {
    QJsonObject o;
    o["bypass"]    = bypass_;
    o["mix"]       = mix_;
    o["attackMs"]  = attMs_;
    o["releaseMs"] = relMs_;
    o["ratio"]     = ratio_;
    o["threshDb"]  = threshDb_;
    o["makeupDb"]  = makeupDb_;
    o["xover"]     = xover_;
    o["sbcOn"]     = sbcOn_;
    o["sbcSpeed"]  = sbcSpeed_;
    QJsonArray bands;
    for (int b = 0; b < kN; ++b) {
        QJsonObject jb;
        jb["thr"] = bandThresh_[b];
        jb["gain"] = bandGain_[b];
        jb["en"]  = bandOn_[b];
        bands.append(jb);
    }
    o["bands"] = bands;
    return o;
}

void CombinatorModel::loadState(const QJsonObject &o) {
    if (o.isEmpty()) return;
    if (o.contains("mix"))       setMix(o["mix"].toDouble(mix_));
    if (o.contains("attackMs"))  setAttackMs(o["attackMs"].toDouble(attMs_));
    if (o.contains("releaseMs")) setReleaseMs(o["releaseMs"].toDouble(relMs_));
    if (o.contains("ratio"))     setRatio(o["ratio"].toDouble(ratio_));
    if (o.contains("threshDb"))  setThreshDb(o["threshDb"].toDouble(threshDb_));
    if (o.contains("makeupDb"))  setMakeupDb(o["makeupDb"].toDouble(makeupDb_));
    if (o.contains("xover"))     setXover(o["xover"].toDouble(xover_));
    if (o.contains("sbcSpeed"))  setSbcSpeed(o["sbcSpeed"].toDouble(sbcSpeed_));
    if (o.contains("sbcOn"))     setSbcEnabled(o["sbcOn"].toBool(sbcOn_));
    const QJsonArray bands = o["bands"].toArray();
    for (int b = 0; b < bands.size() && b < kN; ++b) {
        const QJsonObject jb = bands[b].toObject();
        if (jb.contains("thr"))  setBandThreshDb(b, jb["thr"].toDouble(bandThresh_[b]));
        if (jb.contains("gain")) setBandGainDb(b, jb["gain"].toDouble(bandGain_[b]));
        if (jb.contains("en"))   setBandEnabled(b, jb["en"].toBool(bandOn_[b]));
    }
    if (o.contains("bypass"))    setBypass(o["bypass"].toBool(bypass_));
}

void CombinatorModel::pollMeters() { emit metersChanged(); }

}  // namespace lyra::ui
