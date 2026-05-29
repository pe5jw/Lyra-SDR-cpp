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
#include <limits>

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
constexpr double kMaxDecay      = 0.012;  // max-hold gentle fall (~4 s)
constexpr double kGlowDecay     = 0.160;  // ~0.3 s afterglow
constexpr double kNoiseLeak     = 0.05;   // floor leaks up ~1 dB/s
constexpr double kSmooth        = 0.55;
constexpr int    kHistory       = 90;     // ~4.5 s trail

constexpr auto kKeyStyle    = "meter/style";
constexpr auto kKeyCal      = "meter/calDb";
constexpr auto kKeyPeakHold = "meter/peakHoldMs";
constexpr auto kKeyMaxOn     = "meter/maxPeakEnabled";
constexpr auto kKeyMaxHold   = "meter/maxHoldMs";
constexpr auto kKeySource    = "meter/source";   // active source (Source enum)
constexpr auto kKeyPwrCal    = "meter/pwrCalScale";   // operator cal multiplier
constexpr auto kKeyPwrRated  = "meter/pwrRatedMaxW";  // rated max (red zone start)

// PWR scale conventions.  pwrScaleMaxW = 2 * rated max so the danger
// zone sits at half-scale on the renderer's 0..1 axis.  Smoothing,
// peak-hold, and decay tuned slightly snappier than the RX S-meter
// because TX-watts wants to track speech peaks live; the RX S-meter
// trades response for visual smoothness.
constexpr double kPwrSmooth     = 0.65;        // 0..1; higher = snappier
constexpr double kPwrPeakDecay  = 0.10;
constexpr double kPwrMaxDecay   = 0.012;       // same gentle drop as S-meter
constexpr double kPwrGlowDecay  = 0.18;

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
    peakHoldMs_ = std::clamp(
        s.value(QString::fromLatin1(kKeyPeakHold), 800).toInt(), 100, 5000);
    peakHoldTicks_ = std::max(1, peakHoldMs_ / kTickMs);
    maxPeakEnabled_ = s.value(QString::fromLatin1(kKeyMaxOn), true).toBool();
    maxHoldMs_ = std::clamp(
        s.value(QString::fromLatin1(kKeyMaxHold), 3000).toInt(), 500, 60000);
    maxHoldTicks_ = std::max(1, maxHoldMs_ / kTickMs);
    // Active source (defaults RX_SMETER on first-ever launch — matches
    // the pre-source-plumbing behavior).  Clamped to the valid enum
    // range so a stale or corrupted setting can't surface as an
    // unhandled compute branch.
    {
        const int raw = s.value(QString::fromLatin1(kKeySource),
                                 int(RX_SMETER)).toInt();
        source_ = (raw >= RX_SMETER && raw <= COMP)
                      ? Source(raw)
                      : RX_SMETER;
    }
    pwrCalScale_ = std::clamp(
        s.value(QString::fromLatin1(kKeyPwrCal), 1.0).toDouble(), 0.05, 20.0);
    pwrRatedMaxW_ = std::clamp(
        s.value(QString::fromLatin1(kKeyPwrRated), 5.0).toDouble(), 0.5, 200.0);
    pwrScaleMaxW_ = pwrRatedMaxW_ * 2.0;

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
    // Source-agnostic "danger zone start" position (0..1).  Historical
    // name kept so the QML renderers' zone-coloring logic (S9 boundary
    // → red for the S-meter) generalizes to other sources without
    // renaming the Q_PROPERTY: PWR puts rated-max-W here, SWR puts
    // 2.0 SWR, ALC puts the -3 dB threshold, etc.  S-meter retains
    // the original dBm-derived value; other sources read the cached
    // normDanger_ that their compute fn writes.
    if (source_ == RX_SMETER)
        return (s9Dbm_ - floorDbm_) / (ceilDbm_ - floorDbm_);
    return normDanger_;
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

void MeterModel::setPeakHoldMs(int ms) {
    ms = std::clamp(ms, 100, 5000);
    if (ms == peakHoldMs_) return;
    peakHoldMs_ = ms;
    peakHoldTicks_ = std::max(1, peakHoldMs_ / kTickMs);
    QSettings().setValue(QString::fromLatin1(kKeyPeakHold), peakHoldMs_);
    emit peakHoldChanged();
}

void MeterModel::setMaxPeakEnabled(bool on) {
    if (on == maxPeakEnabled_) return;
    maxPeakEnabled_ = on;
    if (!on) { maxPeak_ = 0.0; maxHoldCtr_ = 0; }
    QSettings().setValue(QString::fromLatin1(kKeyMaxOn), maxPeakEnabled_);
    emit maxPeakCfgChanged();
}

void MeterModel::setMaxHoldMs(int ms) {
    ms = std::clamp(ms, 500, 60000);
    if (ms == maxHoldMs_) return;
    maxHoldMs_ = ms;
    maxHoldTicks_ = std::max(1, maxHoldMs_ / kTickMs);
    QSettings().setValue(QString::fromLatin1(kKeyMaxHold), maxHoldMs_);
    emit maxPeakCfgChanged();
}

void MeterModel::setSource(int s) {
    // Clamp to the valid Source enum range; out-of-range values fall
    // back to RX_SMETER (the safe default).  Reset the per-source
    // hold/decay state so the new source starts from zero instead of
    // inheriting the previous source's level / peak / history (e.g.
    // an S-meter peak would render as a bogus PWR peak on swap).
    if (s < RX_SMETER || s > COMP) s = RX_SMETER;
    if (Source(s) == source_) return;
    source_ = Source(s);
    QSettings().setValue(QString::fromLatin1(kKeySource), int(source_));
    level_ = 0.0;
    peak_  = 0.0;
    maxPeak_ = 0.0;
    glow_  = 0.0;
    holdCtr_ = 0;
    maxHoldCtr_ = 0;
    dispDbm_ = -140.0;
    noiseFloorDbm_ = -140.0;
    noiseLevel_ = 0.0;
    snrText_ = QStringLiteral("—");
    text_ = QStringLiteral("—");
    dbmText_ = QStringLiteral("—");
    std::fill(hist_.begin(), hist_.end(), 0.0);
    for (int i = 0; i < kHistory; ++i) history_[i] = 0.0;
    emit sourceChanged();
    emit updated();   // renderers redraw at zero-state for the new source
}

QVariantList MeterModel::tickMarks() const {
    QVariantList out;
    auto addAt = [&](double pos, const QString &label, bool major) {
        QVariantMap m;
        m[QStringLiteral("pos")]   = std::clamp(pos, 0.0, 1.0);
        m[QStringLiteral("label")] = label;
        m[QStringLiteral("major")] = major;
        out.append(m);
    };
    // S-meter: existing S1/S3/S5/S7/S9/+20/+40/+60 — unchanged.
    if (source_ == RX_SMETER) {
        for (int sl = 1; sl <= 9; sl += 2)
            addAt(normForDbm(s9Dbm_ - (9 - sl) * kDbPerS),
                  QString::number(sl), sl == 9);
        addAt(normForDbm(s9Dbm_ + 20.0), QStringLiteral("+20"), false);
        addAt(normForDbm(s9Dbm_ + 40.0), QStringLiteral("+40"), false);
        addAt(normForDbm(s9Dbm_ + 60.0), QStringLiteral("+60"), false);
        return out;
    }
    if (source_ == PWR) {
        // Watts scale: 0 .. pwrScaleMaxW_ (= 2 * rated max).  Tick
        // spacing scales with the rated max so the labels stay readable
        // whether the operator is on a 5 W bare HL2+ or a 100 W amp.
        // Major ticks at 0, rated, 2*rated; minors at 0.25 / 0.5 /
        // 0.75 / 1.25 / 1.5 / 1.75 multiples — the renderer's "major"
        // flag drives a longer mark + the +20-style emphasis.
        const double r = pwrRatedMaxW_;
        const double m = pwrScaleMaxW_;
        const auto pos = [&](double w) { return std::clamp(w / m, 0.0, 1.0); };
        const auto fmt = [](double w) {
            return (w >= 10.0) ? QString::number(w, 'f', 0)
                                : QString::number(w, 'f', 1);
        };
        addAt(0.0,           QStringLiteral("0"),  true);
        addAt(pos(r * 0.25), fmt(r * 0.25),        false);
        addAt(pos(r * 0.5),  fmt(r * 0.5),         false);
        addAt(pos(r * 0.75), fmt(r * 0.75),        false);
        addAt(pos(r),        fmt(r),               true);   // rated max
        addAt(pos(r * 1.25), fmt(r * 1.25),        false);
        addAt(pos(r * 1.5),  fmt(r * 1.5),         false);
        addAt(pos(r * 1.75), fmt(r * 1.75),        false);
        addAt(1.0,           fmt(m),               true);   // scale max
        return out;
    }
    // SWR / PA / Temp / ALC / MIC / COMP: later commits fill these in.
    return out;
}

void MeterModel::tick() {
    // Dispatch to the source-specific compute.  Each compute fn fills
    // level_/peak_/text_/etc. and emits updated() at the end.  Sources
    // not yet implemented fall through to a passive no-op (render stays
    // at zero state until that source's compute lands in a later
    // commit) — this keeps the foundation commit visually unchanged
    // for the default RX_SMETER case while leaving the dispatch ready.
    switch (source_) {
    case RX_SMETER:   computeSMeter(); return;
    case PWR:         computePwr();    return;
    case SWR:         // task #32
    case PA_CURRENT:  // future HL2-telemetry sources
    case PA_VOLTS:
    case TEMP:
    case ALC:         // task — needs WDSP TXA chain (deferred)
    case MIC:
    case COMP:
        return;
    }
}

void MeterModel::computeSMeter() {
    // RX S-meter compute (extracted from the original tick() body
    // unchanged).  Reads WDSP RXA_S_PK, applies operator cal + the
    // current LNA gain compensation so the reading stays true-to-source
    // regardless of LNA setting, smooths, and maps to the S-unit scale.
    updateScale();

    const double raw = wdsp_ ? wdsp_->sMeterDbm() : -200.0;
    // Reference the reading back to the antenna so it stays TRUE TO SOURCE
    // regardless of LNA: RXA_S_PK is measured after the hardware PGA, so
    // subtract the current LNA gain.  calDb_ then trims the absolute level
    // once (set at any LNA setting); changing LNA no longer moves the S
    // reading.  (Sentinel raw≈−200 when not running → handled downstream.)
    const double lna = stream_ ? static_cast<double>(stream_->lnaGainDb()) : 0.0;
    const double dbm = raw + calDb_ - lna;

    dispDbm_ += kSmooth * (dbm - dispDbm_);
    const double n = normForDbm(dispDbm_);
    level_ = n;

    if (n >= peak_) { peak_ = n; holdCtr_ = peakHoldTicks_; }
    else if (holdCtr_ > 0) { --holdCtr_; }
    else { peak_ = std::max(0.0, peak_ - kPeakDecay); }

    // Max-hold "high-water mark": latch the highest level, hold for
    // maxHoldTicks_, then ease down gently (kMaxDecay << kPeakDecay) so a
    // fleeting DX / QSB crest stays readable long after the fast pip fell.
    if (maxPeakEnabled_) {
        if (n >= maxPeak_) { maxPeak_ = n; maxHoldCtr_ = maxHoldTicks_; }
        else if (maxHoldCtr_ > 0) { --maxHoldCtr_; }
        else { maxPeak_ = std::max(0.0, maxPeak_ - kMaxDecay); }
    } else {
        maxPeak_ = 0.0;
    }

    glow_ = (n >= glow_) ? n : std::max(n, glow_ - kGlowDecay);

    // Noise floor: rolling minimum that leaks up slowly so it follows the
    // band's quiet floor (the original, operator-validated behaviour).
    // THE FIX: skip the −200 "not running / no data" sentinel that
    // sMeterDbm() returns on stop/start + rate changes — feeding it pinned
    // the floor at −200 with no recovery, which is what jammed SNR at 99.
    if (raw > -190.0) {
        if (dispDbm_ < noiseFloorDbm_) noiseFloorDbm_ = dispDbm_;
        else noiseFloorDbm_ = std::min(dispDbm_, noiseFloorDbm_ + kNoiseLeak);
    }
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

void MeterModel::computePwr() {
    // PWR — forward TX power in watts.  Reads HL2Stream::fwdPowerW
    // (provisional ADC → W formula in hl2_stream.cpp; operator's
    // pwrCalScale_ trims it against an external watt-meter anchor) and
    // maps it onto a 0..pwrScaleMaxW_ scale.  Renderer's "danger zone"
    // (the historical normAtS9 binding the QML uses for the red zone)
    // lands at the operator's rated max (pwrRatedMaxW_, default 5 W
    // for a bare HL2+ on-board PA — pwrScaleMaxW_ is 2× that).
    //
    // No noise floor / SNR concept — those are RX-specific.  The text
    // readouts are "X.X W" / "X.XX W peak" so the operator can read
    // the watt-meter without crunching the scale.
    const double raw = stream_ ? stream_->fwdPowerW()
                                : std::numeric_limits<double>::quiet_NaN();
    // Telemetry sentinel: NaN means the slot hasn't arrived yet (stream
    // not running, or pre-first-statsChanged).  Display zero state so
    // the renderer doesn't show stale-bogus levels from a previous
    // source's history.
    const double w = (std::isnan(raw) || raw < 0.0)
                          ? 0.0
                          : raw * pwrCalScale_;

    // Reuse dispDbm_ as the general "smoothed value" cache across all
    // sources (slot is dBm for the S-meter; watts here).  This avoids
    // adding a parallel field family per source while keeping each
    // compute self-contained.
    dispDbm_ += kPwrSmooth * (w - dispDbm_);
    const double n = std::clamp(dispDbm_ / pwrScaleMaxW_, 0.0, 1.0);
    level_ = n;

    if (n >= peak_) { peak_ = n; holdCtr_ = peakHoldTicks_; }
    else if (holdCtr_ > 0) { --holdCtr_; }
    else { peak_ = std::max(0.0, peak_ - kPwrPeakDecay); }

    if (maxPeakEnabled_) {
        if (n >= maxPeak_) { maxPeak_ = n; maxHoldCtr_ = maxHoldTicks_; }
        else if (maxHoldCtr_ > 0) { --maxHoldCtr_; }
        else { maxPeak_ = std::max(0.0, maxPeak_ - kPwrMaxDecay); }
    } else {
        maxPeak_ = 0.0;
    }

    glow_ = (n >= glow_) ? n : std::max(n, glow_ - kPwrGlowDecay);

    // No noise-floor marker for TX-side sources.  Set noiseLevel_ to
    // 0 so the renderer's `> 0.001` gate skips the marker draw cleanly.
    noiseLevel_ = 0.0;
    snrText_.clear();

    // Danger-zone position for the renderer's red-zone coloring.  Equals
    // pwrRatedMaxW_ / pwrScaleMaxW_ = 0.5 with the default 5 W rated /
    // 10 W scale (rated max sits at half-scale visually).
    normDanger_ = std::clamp(pwrRatedMaxW_ / pwrScaleMaxW_, 0.0, 1.0);

    hist_.push_back(n);
    if (int(hist_.size()) > kHistory) hist_.pop_front();
    for (int i = 0; i < kHistory; ++i) history_[i] = hist_[i];

    // Text formatting: < 10 W shows one decimal (the QRP regime where
    // 0.1 W matters); >= 10 W shows whole watts.
    const double dispW = std::max(0.0, dispDbm_);
    text_ = (dispW < 10.0)
                ? QStringLiteral("%1 W").arg(dispW, 0, 'f', 1)
                : QStringLiteral("%1 W").arg(std::lround(dispW));
    dbmText_.clear();

    emit updated();
}

} // namespace lyra::ui
