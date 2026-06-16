// Lyra — SpeechModel implementation.  See speechmodel.h.

#include "speechmodel.h"

namespace lyra::ui {

std::atomic<lyra::dsp::SpeechProcessor *> SpeechModel::s_txEngine{nullptr};

SpeechModel::SpeechModel(QObject *parent) : QObject(parent) {
    sp_.setSampleRate(48000.0);
    // Push the model defaults into the engine so the two agree at startup.
    sp_.setGateThreshDb(gateThreshDb_);
    sp_.setGateRangeDb(gateRangeDb_);
    sp_.setGateHoldMs(gateHoldMs_);
    sp_.setAgcTargetDb(agcTargetDb_);
    sp_.setAgcMaxGainDb(agcMaxGainDb_);
    sp_.setDeessFreqHz(deessFreqHz_);
    sp_.setDeessThreshDb(deessThreshDb_);
    sp_.setDeessRangeDb(deessRangeDb_);
    s_txEngine.store(&sp_, std::memory_order_release);
}

SpeechModel::~SpeechModel() {
    s_txEngine.store(nullptr, std::memory_order_release);
}

void SpeechModel::setGateEnabled(bool on) {
    if (on == gateEnabled_) return;
    gateEnabled_ = on; sp_.setGateEnabled(on); emit changed();
}
void SpeechModel::setGateThreshDb(double db) {
    if (db == gateThreshDb_) return;
    gateThreshDb_ = db; sp_.setGateThreshDb(db); emit changed();
}
void SpeechModel::setGateRangeDb(double db) {
    if (db == gateRangeDb_) return;
    gateRangeDb_ = db; sp_.setGateRangeDb(db); emit changed();
}
void SpeechModel::setGateHoldMs(double ms) {
    if (ms == gateHoldMs_) return;
    gateHoldMs_ = ms; sp_.setGateHoldMs(ms); emit changed();
}

void SpeechModel::setAgcEnabled(bool on) {
    if (on == agcEnabled_) return;
    agcEnabled_ = on; sp_.setAgcEnabled(on); emit changed();
}
void SpeechModel::setAgcTargetDb(double db) {
    if (db == agcTargetDb_) return;
    agcTargetDb_ = db; sp_.setAgcTargetDb(db); emit changed();
}
void SpeechModel::setAgcMaxGainDb(double db) {
    if (db == agcMaxGainDb_) return;
    agcMaxGainDb_ = db; sp_.setAgcMaxGainDb(db); emit changed();
}
void SpeechModel::setDeessEnabled(bool on) {
    if (on == deessEnabled_) return;
    deessEnabled_ = on; sp_.setDeessEnabled(on); emit changed();
}
void SpeechModel::setDeessFreqHz(double hz) {
    if (hz == deessFreqHz_) return;
    deessFreqHz_ = hz; sp_.setDeessFreqHz(hz); emit changed();
}
void SpeechModel::setDeessThreshDb(double db) {
    if (db == deessThreshDb_) return;
    deessThreshDb_ = db; sp_.setDeessThreshDb(db); emit changed();
}
void SpeechModel::setDeessRangeDb(double db) {
    if (db == deessRangeDb_) return;
    deessRangeDb_ = db; sp_.setDeessRangeDb(db); emit changed();
}

void SpeechModel::txProcessCb(int nSamples, double *iqPairs) {
    if (auto *e = s_txEngine.load(std::memory_order_acquire))
        e->processInterleaved(iqPairs, nSamples);
}

}  // namespace lyra::ui
