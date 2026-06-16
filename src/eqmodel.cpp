// Lyra — EqModel implementation.  See eqmodel.h.

#include "eqmodel.h"

#include <algorithm>

namespace lyra::ui {

namespace {
using DType = lyra::dsp::ParamEq::Type;
constexpr double kFreqMin = 20.0;
constexpr double kFreqMax = 20000.0;
constexpr double kGainMax = 15.0;     // ±15 dB drag range (matches the curve axis)
constexpr double kQMin = 0.2;
constexpr double kQMax = 10.0;
}  // namespace

// Active engine for the wire-layer TX pump (txProcessCb).  One EqModel
// exists for the app lifetime (MainWindow member); the TX thread reads
// this lock-free.
std::atomic<lyra::dsp::ParamEq *> EqModel::s_txEngine{nullptr};

EqModel::EqModel(QObject *parent) : QObject(parent) {
    eq_.setSampleRate(48000.0);   // HL2 codec / TX rack rate (mic is 48 kHz)
    s_txEngine.store(&eq_, std::memory_order_release);
}

EqModel::~EqModel() {
    // Stop routing TX audio through this engine before it's destroyed.
    s_txEngine.store(nullptr, std::memory_order_release);
}

void EqModel::txProcessCb(int nSamples, double *iqPairs) {
    if (auto *e = s_txEngine.load(std::memory_order_acquire))
        e->processInterleaved(iqPairs, nSamples);
}

void EqModel::setBypass(bool on) {
    if (on == bypass_) return;
    bypass_ = on;
    eq_.setBypass(on);
    emit bypassChanged();
    emit bandsChanged();          // curve dims/flattens
}

void EqModel::setMakeupDb(double db) {
    db = std::clamp(db, -24.0, 24.0);
    if (db == makeupDb_) return;
    makeupDb_ = db;
    eq_.setMakeupDb(db);
    emit makeupDbChanged();
    emit bandsChanged();
}

void EqModel::setSelectedBand(int i) {
    if (i == selected_) return;
    selected_ = i;
    emit selectedBandChanged();
}

bool   EqModel::bandEnabled(int i) const { return valid(i) && eq_.band(i).enabled; }
int    EqModel::bandType(int i) const {
    return valid(i) ? static_cast<int>(eq_.band(i).type) : 0;
}
double EqModel::bandFreq(int i) const { return valid(i) ? eq_.band(i).freqHz : 0.0; }
double EqModel::bandGain(int i) const { return valid(i) ? eq_.band(i).gainDb : 0.0; }
double EqModel::bandQ(int i) const { return valid(i) ? eq_.band(i).q : 1.0; }

void EqModel::setBandEnabled(int i, bool on) {
    if (!valid(i)) return;
    auto b = eq_.band(i);
    if (b.enabled == on) return;
    b.enabled = on;
    eq_.setBand(i, b);
    emit bandsChanged();
}

void EqModel::setBandType(int i, int type) {
    if (!valid(i) || type < 0 || type > static_cast<int>(DType::Notch)) return;
    auto b = eq_.band(i);
    b.type = static_cast<DType>(type);
    eq_.setBand(i, b);
    emit bandsChanged();
}

void EqModel::setBandFreq(int i, double hz) {
    if (!valid(i)) return;
    hz = std::clamp(hz, kFreqMin, kFreqMax);
    auto b = eq_.band(i);
    b.freqHz = hz;
    eq_.setBand(i, b);
    emit bandsChanged();
}

void EqModel::setBandGain(int i, double db) {
    if (!valid(i)) return;
    db = std::clamp(db, -kGainMax, kGainMax);
    auto b = eq_.band(i);
    b.gainDb = db;
    eq_.setBand(i, b);
    emit bandsChanged();
}

void EqModel::setBandQ(int i, double q) {
    if (!valid(i)) return;
    q = std::clamp(q, kQMin, kQMax);
    auto b = eq_.band(i);
    b.q = q;
    eq_.setBand(i, b);
    emit bandsChanged();
}

void EqModel::resetBand(int i) {
    if (!valid(i)) return;
    lyra::dsp::ParamEq fresh;     // its constructor seeds the default layout
    eq_.setBand(i, fresh.band(i));
    emit bandsChanged();
}

double EqModel::magnitudeDb(double freqHz) const { return eq_.magnitudeDb(freqHz); }

QString EqModel::typeName(int type) const {
    switch (static_cast<DType>(type)) {
    case DType::Peak:      return QStringLiteral("Peak");
    case DType::LowShelf:  return QStringLiteral("Lo-Shelf");
    case DType::HighShelf: return QStringLiteral("Hi-Shelf");
    case DType::LowPass:   return QStringLiteral("Lo-Pass");
    case DType::HighPass:  return QStringLiteral("Hi-Pass");
    case DType::BandPass:  return QStringLiteral("Bandpass");
    case DType::Notch:     return QStringLiteral("Notch");
    }
    return QStringLiteral("?");
}

bool EqModel::typeUsesGain(int type) const {
    const auto t = static_cast<DType>(type);
    // Peak + shelves boost/cut; Notch is a cut whose DEPTH is its gain, so
    // its node rides gain too (drag down = deeper).  LP/HP/BP are slope/
    // width only — no gain handle.
    return t == DType::Peak || t == DType::LowShelf
           || t == DType::HighShelf || t == DType::Notch;
}

}  // namespace lyra::ui
