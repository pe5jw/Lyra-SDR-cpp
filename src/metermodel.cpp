// Lyra — RX signal-strength meter model.  See metermodel.h.
//
// Calibration mirrors Thetis: the source is WDSP's in-passband
// RXA_S_PK meter (WdspEngine::sMeterDbm), an operator trim (calDb)
// stands in for Thetis's per-radio RXMeterCalOffset (+ the fixed HL2
// LNA offset, until an LNA-gain control is ported), and dBm→S-units
// uses Thetis's SMeterFromDBM table with the 30 MHz HF/VHF split
// (S9 = -73 dBm below 30 MHz, -93 dBm above).

#include "metermodel.h"

#include "hl2_stream.h"
#include "wdsp_engine.h"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace lyra::ui {

namespace {
constexpr double kDbPerS  = 6.0;
constexpr double kS9Hf    = -73.0;   // S9 below 30 MHz
constexpr double kS9Vhf   = -93.0;   // S9 at/above 30 MHz
// Scale endpoints (S1 floor … S9+60 ceiling) for each regime.
constexpr double kFloorHf = -124.0, kCeilHf = -13.0;
constexpr double kFloorVhf = -144.0, kCeilVhf = -33.0;
constexpr double kS9FreqHz = 30.0e6;

constexpr int    kTickMs        = 50;     // 20 Hz
constexpr int    kPeakHoldTicks = 16;     // ~0.8 s dwell
constexpr double kPeakDecay     = 0.080;  // ~0.6 s fall
constexpr double kGlowDecay     = 0.160;  // ~0.3 s afterglow
constexpr double kNoiseLeak     = 0.05;   // floor leaks up ~1 dB/s
constexpr double kSmooth        = 0.55;
constexpr int    kHistory       = 90;     // ~4.5 s trail

constexpr auto kKeyStyle = "meter/style";
constexpr auto kKeyCal   = "meter/calDb";

// Thetis SMeterFromDBM, compact form. Ascending (upperBound, label):
// return the first row whose dbm <= upperBound.
struct SRow { double upper; const char *label; };
const SRow kHfRows[] = {
    {-124, "S0"}, {-118, "S1"}, {-112, "S2"}, {-106, "S3"}, {-100, "S4"},
    {-94,  "S5"}, {-88,  "S6"}, {-82,  "S7"}, {-76,  "S8"}, {-70,  "S9"},
    {-66, "S9+5"}, {-60, "S9+10"}, {-56, "S9+15"}, {-46, "S9+20"},
    {-36, "S9+30"}, {-26, "S9+40"}, {-16, "S9+50"},
};
const SRow kVhfRows[] = {
    {-144, "S0"}, {-138, "S1"}, {-132, "S2"}, {-126, "S3"}, {-120, "S4"},
    {-114, "S5"}, {-108, "S6"}, {-102, "S7"}, {-96,  "S8"}, {-90,  "S9"},
    {-86, "S9+5"}, {-80, "S9+10"}, {-76, "S9+15"}, {-66, "S9+20"},
    {-56, "S9+30"}, {-46, "S9+40"}, {-36, "S9+50"},
};
}  // namespace

MeterModel::MeterModel(lyra::ipc::HL2Stream *stream,
                       lyra::dsp::WdspEngine *wdsp, QObject *parent)
    : QObject(parent), stream_(stream), wdsp_(wdsp) {
    QSettings s;
    style_ = s.value(QString::fromLatin1(kKeyStyle), 0).toInt();
    calDb_ = s.value(QString::fromLatin1(kKeyCal), 0.0).toDouble();

    hist_.assign(kHistory, 0.0);
    history_.reserve(kHistory);
    for (int i = 0; i < kHistory; ++i) history_.append(0.0);

    updateScale();
    connect(&timer_, &QTimer::timeout, this, &MeterModel::tick);
    timer_.start(kTickMs);
}

void MeterModel::updateScale() {
    const double f = stream_ ? double(stream_->rx1FreqHz()) : 0.0;
    aboveS9_ = (f >= kS9FreqHz);
    floorDbm_ = aboveS9_ ? kFloorVhf : kFloorHf;
    ceilDbm_  = aboveS9_ ? kCeilVhf  : kCeilHf;
    s9Dbm_    = aboveS9_ ? kS9Vhf    : kS9Hf;
}

double MeterModel::normForDbm(double dbm) const {
    return std::clamp((dbm - floorDbm_) / (ceilDbm_ - floorDbm_), 0.0, 1.0);
}

double MeterModel::normAtS9() const {
    return (s9Dbm_ - floorDbm_) / (ceilDbm_ - floorDbm_);
}

QString MeterModel::sLabel(double dbm) const {
    const SRow *rows = aboveS9_ ? kVhfRows : kHfRows;
    const int n = aboveS9_ ? int(std::size(kVhfRows)) : int(std::size(kHfRows));
    for (int i = 0; i < n; ++i)
        if (dbm <= rows[i].upper) return QString::fromLatin1(rows[i].label);
    return QStringLiteral("S9+60");
}

void MeterModel::setStyle(int s) {
    s = (s != 0) ? 1 : 0;
    if (s == style_) return;
    style_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyStyle), style_);
    emit styleChanged();
}

void MeterModel::setCalDb(double d) {
    d = std::clamp(d, -60.0, 60.0);
    if (std::abs(d - calDb_) < 1e-9) return;
    calDb_ = d;
    QSettings().setValue(QString::fromLatin1(kKeyCal), calDb_);
    emit calChanged();
}

QVariantList MeterModel::tickMarks() const {
    QVariantList out;
    auto add = [&](double dbm, const QString &label, bool major) {
        QVariantMap m;
        m[QStringLiteral("pos")]   = normForDbm(dbm);
        m[QStringLiteral("label")] = label;
        m[QStringLiteral("major")] = major;
        out.append(m);
    };
    for (int s = 1; s <= 9; s += 2)
        add(s9Dbm_ - (9 - s) * kDbPerS, QString::number(s), s == 9);
    add(s9Dbm_ + 20.0, QStringLiteral("+20"), false);
    add(s9Dbm_ + 40.0, QStringLiteral("+40"), false);
    add(s9Dbm_ + 60.0, QStringLiteral("+60"), false);
    return out;
}

void MeterModel::tick() {
    updateScale();

    const double raw = wdsp_ ? wdsp_->sMeterDbm() : -200.0;
    const double dbm = raw + calDb_;

    dispDbm_ += kSmooth * (dbm - dispDbm_);
    const double n = normForDbm(dispDbm_);
    level_ = n;

    if (n >= peak_) { peak_ = n; holdCtr_ = kPeakHoldTicks; }
    else if (holdCtr_ > 0) { --holdCtr_; }
    else { peak_ = std::max(0.0, peak_ - kPeakDecay); }

    glow_ = (n >= glow_) ? n : std::max(n, glow_ - kGlowDecay);

    // Noise floor: track the level's rolling minimum, leaking up slowly
    // so it follows a rising floor instead of sticking at an old low.
    if (dispDbm_ < noiseFloorDbm_) noiseFloorDbm_ = dispDbm_;
    else noiseFloorDbm_ = std::min(dispDbm_, noiseFloorDbm_ + kNoiseLeak);
    noiseLevel_ = normForDbm(noiseFloorDbm_);
    const double snr = dispDbm_ - noiseFloorDbm_;
    snrText_ = (level_ <= 0.01 || snr < 1.0)
                   ? QStringLiteral("—")
                   : QStringLiteral("SNR %1 dB").arg(std::lround(std::min(snr, 99.0)));

    hist_.push_back(n);
    if (int(hist_.size()) > kHistory) hist_.pop_front();
    for (int i = 0; i < kHistory; ++i) history_[i] = hist_[i];

    text_ = sLabel(dispDbm_);
    dbmText_ = QStringLiteral("%1 dBm").arg(dispDbm_, 0, 'f', 0);

    emit updated();
}

} // namespace lyra::ui
