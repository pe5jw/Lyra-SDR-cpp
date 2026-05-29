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
constexpr auto kKeyRxSource  = "meter/rxSource"; // operator's RX-state preference
constexpr auto kKeyTxSource  = "meter/txSource"; // operator's TX-state preference
constexpr auto kKeyPwrCal    = "meter/pwrCalScale";   // operator cal multiplier
constexpr auto kKeyPwrRated  = "meter/pwrRatedMaxW";  // rated max (red zone start)
constexpr auto kKeyTxSecond  = "meter/txSecondary";   // TX secondary readout source
constexpr auto kKeyTxSecond2 = "meter/txSecondary2";  // TX second secondary readout source

// PWR scale conventions.  pwrScaleMaxW = 2 * rated max so the danger
// zone sits at half-scale on the renderer's 0..1 axis.  Smoothing,
// peak-hold, and decay tuned slightly snappier than the RX S-meter
// because TX-watts wants to track speech peaks live; the RX S-meter
// trades response for visual smoothness.
constexpr double kPwrSmooth     = 0.65;        // 0..1; higher = snappier
constexpr double kPwrPeakDecay  = 0.10;
constexpr double kPwrMaxDecay   = 0.012;       // same gentle drop as S-meter
constexpr double kPwrGlowDecay  = 0.18;

// SWR scale conventions.  Below this guard-band fwd power the
// reflection-coefficient computation is dominated by ADC noise and
// would render garbage SWR (e.g. 8:1 on a perfectly matched dummy
// load) — the model displays "—" instead, matching the convention
// every commercial transceiver uses for low-power SWR readouts.
// Above the scale ceiling the meter pegs and the text shows "≥3:1".
constexpr double kSwrGuardW     = 0.5;         // fwd < this → no reading
constexpr double kSwrScaleMin   = 1.0;         // perfect match
constexpr double kSwrScaleMax   = 3.0;         // peg-and-show-≥ above this
constexpr double kSwrDanger     = 2.0;         // industry "high" threshold
constexpr double kSwrSmooth     = 0.40;        // calmer than PWR (operator
                                                // wants steady reading,
                                                // not speech-peak chasing)
constexpr double kSwrPeakDecay  = 0.06;
constexpr double kSwrGlowDecay  = 0.12;

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
    style_ = std::clamp(s.value(QString::fromLatin1(kKeyStyle), 0).toInt(),
                        0, 2);
    calDb_ = s.value(QString::fromLatin1(kKeyCal), 0.0).toDouble();
    peakHoldMs_ = std::clamp(
        s.value(QString::fromLatin1(kKeyPeakHold), 800).toInt(), 100, 5000);
    peakHoldTicks_ = std::max(1, peakHoldMs_ / kTickMs);
    maxPeakEnabled_ = s.value(QString::fromLatin1(kKeyMaxOn), true).toBool();
    maxHoldMs_ = std::clamp(
        s.value(QString::fromLatin1(kKeyMaxHold), 3000).toInt(), 500, 60000);
    maxHoldTicks_ = std::max(1, maxHoldMs_ / kTickMs);
    // RX-state and TX-state source preferences.  Defaults RX_SMETER and
    // PWR respectively on first-ever launch — the obvious sane choices.
    // Clamped to the valid enum range so a stale or corrupted setting
    // can't surface as an unhandled compute branch.  Active source is
    // derived: rxSource at rest, txSource when the wire MOX bit is set.
    auto clampSrc = [](int raw, Source fallback) {
        return (raw >= RX_SMETER && raw <= COMP) ? Source(raw) : fallback;
    };
    rxSource_ = clampSrc(s.value(QString::fromLatin1(kKeyRxSource),
                                  int(RX_SMETER)).toInt(),
                          RX_SMETER);
    txSource_ = clampSrc(s.value(QString::fromLatin1(kKeyTxSource),
                                  int(PWR)).toInt(),
                          PWR);
    // Initialize the active source from the live MOX state if we already
    // have a stream (we do — passed via the ctor arg).  moxActive is
    // false at construction time (the stream isn't open yet), so this
    // boils down to rxSource_ in practice — but the conditional keeps
    // the derivation rule explicit.
    source_ = (stream_ && stream_->moxActive()) ? txSource_ : rxSource_;
    pwrCalScale_ = std::clamp(
        s.value(QString::fromLatin1(kKeyPwrCal), 1.0).toDouble(), 0.05, 20.0);
    pwrRatedMaxW_ = std::clamp(
        s.value(QString::fromLatin1(kKeyPwrRated), 5.0).toDouble(), 0.5, 200.0);
    pwrScaleMaxW_ = pwrRatedMaxW_ * 2.0;
    // TX secondary digital readout default = -1 (none).  Clamp to the
    // valid Source enum range (or -1) so a stale value can't surface
    // as garbage text in the renderer's snrText slot.
    {
        const int raw = s.value(QString::fromLatin1(kKeyTxSecond), -1).toInt();
        txSecondary_ = (raw == -1 || (raw >= RX_SMETER && raw <= COMP))
                           ? raw
                           : -1;
    }
    {
        const int raw = s.value(QString::fromLatin1(kKeyTxSecond2), -1).toInt();
        txSecondary2_ = (raw == -1 || (raw >= RX_SMETER && raw <= COMP))
                            ? raw
                            : -1;
    }

    hist_.assign(kHistory, 0.0);
    history_.reserve(kHistory);
    for (int i = 0; i < kHistory; ++i) history_.append(0.0);

    updateScale();
    connect(&timer_, &QTimer::timeout, this, &MeterModel::tick);
    timer_.start(kTickMs);

    // MOX-edge auto-swap (task #33).  When the wire MOX bit settles
    // true (post TR-delay), swap to the operator's TX preference.
    // When it clears (post ptt_out_delay), swap back to the RX
    // preference.  Goes through setSource() so the per-source state
    // reset (peak/hold/history zeroing) happens on every swap — a
    // stale RX peak doesn't render as a bogus PWR peak across the
    // edge.  No direct QSettings write on the swap itself: the
    // operator's PREFS persist (rxSource/txSource), the CURRENT
    // displayed source is derived from them + the MOX state.
    if (stream_) {
        connect(stream_, &lyra::ipc::HL2Stream::moxActiveChanged, this,
                [this](bool on) {
                    const Source target = on ? txSource_ : rxSource_;
                    if (target == source_) return;
                    // Swap WITHOUT touching meter/source persistence —
                    // that key is no longer used (rxSource/txSource
                    // are the canonical prefs).  Reuse the body of
                    // setSource minus the QSettings write.
                    source_ = target;
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
                    secondary2Text_.clear();
                    text_ = QStringLiteral("—");
                    dbmText_ = QStringLiteral("—");
                    std::fill(hist_.begin(), hist_.end(), 0.0);
                    for (int i = 0; i < kHistory; ++i) history_[i] = 0.0;
                    emit sourceChanged();
                    emit updated();
                });
    }
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
    // 0=HorizonArc (default), 1=PlasmaBar, 2=VerticalLadder (Multi).
    s = std::clamp(s, 0, 2);
    if (s == style_) return;
    style_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyStyle), style_);
    if (style_ == 2) {
        // Populate immediately so the renderer paints real rows on the
        // first frame instead of waiting for the next tick.
        buildLadderRows();
    } else {
        ladderRows_.clear();
    }
    emit styleChanged();
    emit updated();
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
    // Click-to-cycle / direct-set entry point (task #35 will hook the
    // meter-face click here).  Updates the appropriate preference slot
    // — RX or TX based on the live MOX state — so the operator's
    // choice persists into the right per-state pref, then derives the
    // active source.  Falls back to RX_SMETER for out-of-range values.
    if (s < RX_SMETER || s > COMP) s = RX_SMETER;
    const Source ns = Source(s);
    const bool moxOn = stream_ && stream_->moxActive();
    if (moxOn) {
        if (txSource_ != ns) {
            txSource_ = ns;
            QSettings().setValue(QString::fromLatin1(kKeyTxSource), int(ns));
            emit txSourceChanged();
        }
    } else {
        if (rxSource_ != ns) {
            rxSource_ = ns;
            QSettings().setValue(QString::fromLatin1(kKeyRxSource), int(ns));
            emit rxSourceChanged();
        }
    }
    if (ns == source_) return;
    source_ = ns;
    // Reset per-source state so the new source starts from zero (a
    // stale S-meter peak would render as a bogus PWR peak on swap).
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
    secondary2Text_.clear();
    text_ = QStringLiteral("—");
    dbmText_ = QStringLiteral("—");
    std::fill(hist_.begin(), hist_.end(), 0.0);
    for (int i = 0; i < kHistory; ++i) history_[i] = 0.0;
    emit sourceChanged();
    emit updated();
}

void MeterModel::setRxSource(int s) {
    if (s < RX_SMETER || s > COMP) s = RX_SMETER;
    const Source ns = Source(s);
    if (ns == rxSource_) return;
    rxSource_ = ns;
    QSettings().setValue(QString::fromLatin1(kKeyRxSource), int(ns));
    emit rxSourceChanged();
    // If currently at rest, the new RX pref takes effect immediately —
    // route through setSource so the per-source state reset happens.
    if (stream_ && !stream_->moxActive() && source_ != ns)
        setSource(int(ns));
}

void MeterModel::setTxSource(int s) {
    if (s < RX_SMETER || s > COMP) s = RX_SMETER;
    const Source ns = Source(s);
    if (ns == txSource_) return;
    txSource_ = ns;
    QSettings().setValue(QString::fromLatin1(kKeyTxSource), int(ns));
    emit txSourceChanged();
    // If currently in MOX, the new TX pref takes effect immediately.
    if (stream_ && stream_->moxActive() && source_ != ns)
        setSource(int(ns));
}

void MeterModel::setPwrRatedMaxW(double w) {
    // 0.5 W floor catches the QRP regime; 200 W ceiling covers
    // operators on legal-limit amps comfortably with the
    // scale-max = 2 * rated convention (~400 W full scale).
    w = std::clamp(w, 0.5, 200.0);
    if (std::abs(w - pwrRatedMaxW_) < 1e-6) return;
    pwrRatedMaxW_ = w;
    pwrScaleMaxW_ = w * 2.0;
    QSettings().setValue(QString::fromLatin1(kKeyPwrRated), pwrRatedMaxW_);
    emit pwrCalChanged();
    // Force a re-emit so the renderer redraws the scale ticks +
    // danger-zone position with the new rated max even before the
    // next tick lands.
    if (source_ == PWR) emit updated();
}

void MeterModel::setTxSecondary(int s) {
    // -1 (none) or a valid Source enum value.  Out-of-range falls
    // back to -1 (hide) rather than RX_SMETER, because the operator's
    // intent for an invalid secondary is "show nothing" — picking
    // RX_SMETER silently would surprise them with an S-meter readout
    // under their PWR meter.
    if (s != -1 && (s < RX_SMETER || s > COMP)) s = -1;
    if (s == txSecondary_) return;
    txSecondary_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyTxSecond), s);
    emit txSecondaryChanged();
    // Re-emit so the renderer's snrText slot clears or refreshes on
    // the current frame instead of waiting for the next tick.
    emit updated();
}

void MeterModel::setTxSecondary2(int s) {
    // Sibling of setTxSecondary — second small line under the primary,
    // brings TX into RX-parity 3-line layout.  Same -1-or-Source-enum
    // contract.
    if (s != -1 && (s < RX_SMETER || s > COMP)) s = -1;
    if (s == txSecondary2_) return;
    txSecondary2_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyTxSecond2), s);
    emit txSecondary2Changed();
    emit updated();
}

QString MeterModel::formatSecondaryText(int src) const {
    // Format a one-line digital readout for the given source — used to
    // populate snrText_ as the "small text under the main needle" slot
    // when a TX primary has txSecondary_ set.  Pure read of the
    // underlying stream_ getters; never modifies model state.  Each
    // case mirrors the primary compute's text format so the operator
    // reads identical numbers (e.g. PWR-as-secondary uses the same
    // pwrCalScale_ trim).  NaN / sentinel values render as "—".
    if (!stream_) return QString();
    switch (src) {
    case PWR: {
        const double raw = stream_->fwdPowerW();
        if (std::isnan(raw) || raw < 0.0) return QStringLiteral("PWR —");
        const double w = raw * pwrCalScale_;
        return (w < 10.0)
                   ? QStringLiteral("PWR %1 W").arg(w, 0, 'f', 1)
                   : QStringLiteral("PWR %1 W").arg(std::lround(w));
    }
    case SWR: {
        const double fwd = stream_->fwdPowerW();
        const double rev = stream_->revPowerW();
        if (std::isnan(fwd) || std::isnan(rev) || fwd < kSwrGuardW)
            return QStringLiteral("SWR —");
        const double r = std::clamp(rev / std::max(fwd, 1e-9), 0.0, 0.999);
        const double rho = std::sqrt(r);
        const double swr = std::clamp((1.0 + rho) / std::max(1.0 - rho, 1e-9),
                                       kSwrScaleMin, 20.0);
        return (swr >= kSwrScaleMax)
                   ? QStringLiteral("SWR ≥%1:1").arg(kSwrScaleMax, 0, 'f', 1)
                   : QStringLiteral("SWR %1:1").arg(swr, 0, 'f', 2);
    }
    case PA_CURRENT: {
        const double a = stream_->paCurrentA();
        return std::isnan(a)
                   ? QStringLiteral("PA —")
                   : QStringLiteral("PA %1 A").arg(a, 0, 'f', 2);
    }
    case PA_VOLTS: {
        const double v = stream_->hl2SupplyV();
        return std::isnan(v)
                   ? QStringLiteral("V —")
                   : QStringLiteral("V %1 V").arg(v, 0, 'f', 1);
    }
    case TEMP: {
        const double c = stream_->hl2TempC();
        return std::isnan(c)
                   ? QStringLiteral("T —")
                   : QStringLiteral("T %1 °C").arg(c, 0, 'f', 1);
    }
    case ALC:
    case MIC:
    case COMP:
        // Need WDSP TXA chain — placeholder until those land.
        return QString();
    case RX_SMETER:
    default:
        return QString();
    }
}

void MeterModel::setPwrCalScale(double s) {
    // Wide range covers a wide ADC-to-watt mismatch — operators on
    // exotic forward-power bridges may need to trim quite far from 1.0.
    s = std::clamp(s, 0.05, 20.0);
    if (std::abs(s - pwrCalScale_) < 1e-6) return;
    pwrCalScale_ = s;
    QSettings().setValue(QString::fromLatin1(kKeyPwrCal), pwrCalScale_);
    emit pwrCalChanged();
    if (source_ == PWR) emit updated();
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
    if (source_ == SWR) {
        // SWR scale: 1.0 (perfect match) on the left, 3.0+ on the right
        // (above 3:1 the meter pegs).  Danger threshold at 2.0 sits at
        // (2-1)/(3-1) = 0.5 on the renderer's 0..1 axis — half-scale,
        // matching the PWR convention.  Operator-relevant SWR values
        // (1.0/1.2/1.5/1.7/2.0/2.5/3.0) all get their own tick.
        const auto pos = [](double swr) {
            return std::clamp((swr - kSwrScaleMin) /
                              (kSwrScaleMax - kSwrScaleMin), 0.0, 1.0);
        };
        addAt(pos(1.0), QStringLiteral("1.0"), true);
        addAt(pos(1.2), QStringLiteral("1.2"), false);
        addAt(pos(1.5), QStringLiteral("1.5"), false);
        addAt(pos(1.7), QStringLiteral("1.7"), false);
        addAt(pos(2.0), QStringLiteral("2.0"), true);   // danger threshold
        addAt(pos(2.5), QStringLiteral("2.5"), false);
        addAt(pos(3.0), QStringLiteral("3.0"), true);
        return out;
    }
    // PA_CURRENT / PA_VOLTS / TEMP / ALC / MIC / COMP: later commits.
    return out;
}

void MeterModel::ladderRowFor(int src, double *level, double *danger) const {
    // Per-source level + danger threshold for the Ladder rows.  Each
    // source gets its own normalization so the bar fill reads "as a
    // fraction of operator-relevant range" — e.g. SWR's 0..1 maps over
    // the 1.0..3.0 axis (with 2.0 = danger at 0.5), PWR over
    // 0..pwrScaleMaxW_ (with rated max at half-scale).
    *level = 0.0;
    *danger = 1.0;  // off-scale = no red zone
    if (!stream_) return;
    switch (src) {
    case PWR: {
        const double raw = stream_->fwdPowerW();
        const double w = (std::isnan(raw) || raw < 0.0) ? 0.0
                                                        : raw * pwrCalScale_;
        *level = std::clamp(w / std::max(pwrScaleMaxW_, 1e-9), 0.0, 1.0);
        *danger = std::clamp(pwrRatedMaxW_ / std::max(pwrScaleMaxW_, 1e-9),
                              0.0, 1.0);
        return;
    }
    case SWR: {
        const double fwd = stream_->fwdPowerW();
        const double rev = stream_->revPowerW();
        if (std::isnan(fwd) || std::isnan(rev) || fwd < kSwrGuardW) return;
        const double r = std::clamp(rev / std::max(fwd, 1e-9), 0.0, 0.999);
        const double rho = std::sqrt(r);
        const double swr = std::clamp((1.0 + rho) / std::max(1.0 - rho, 1e-9),
                                       kSwrScaleMin, kSwrScaleMax);
        *level = std::clamp((swr - kSwrScaleMin) /
                            std::max(kSwrScaleMax - kSwrScaleMin, 1e-9),
                            0.0, 1.0);
        *danger = std::clamp((kSwrDanger - kSwrScaleMin) /
                              std::max(kSwrScaleMax - kSwrScaleMin, 1e-9),
                              0.0, 1.0);
        return;
    }
    case PA_CURRENT: {
        // HL2+ scale: idle ~0.2 A, full-tune anchor ~1.8 A.  Run the
        // row out to 3 A (covers external-PA telemetry too); danger
        // at 2.5 A (above operator's normal full-drive draw).
        const double a = stream_->paCurrentA();
        if (std::isnan(a)) return;
        constexpr double kPAFullScale = 3.0;
        constexpr double kPADanger    = 2.5;
        *level = std::clamp(a / kPAFullScale, 0.0, 1.0);
        *danger = kPADanger / kPAFullScale;
        return;
    }
    case PA_VOLTS: {
        // Center the bar on the 12-13 V nominal HL2 supply; full scale
        // 16 V leaves headroom for higher-VDD external supplies.
        const double v = stream_->hl2SupplyV();
        if (std::isnan(v)) return;
        constexpr double kVFullScale = 16.0;
        constexpr double kVDanger    = 14.0;
        *level = std::clamp(v / kVFullScale, 0.0, 1.0);
        *danger = kVDanger / kVFullScale;
        return;
    }
    case TEMP: {
        // HL2 board temp: idle ~25 °C, full-tune ~31 °C; red zone at
        // 60 °C (well below the gateware thermal-cutoff floor).
        const double c = stream_->hl2TempC();
        if (std::isnan(c)) return;
        constexpr double kTempFullScale = 80.0;
        constexpr double kTempDanger    = 60.0;
        *level = std::clamp(c / kTempFullScale, 0.0, 1.0);
        *danger = kTempDanger / kTempFullScale;
        return;
    }
    case RX_SMETER: {
        // RX-mode hero row: reuse the active S-meter level/danger so
        // the Ladder's primary line matches what the Arc/Bar would show.
        *level = level_;
        *danger = normAtS9();
        return;
    }
    default:
        return;
    }
}

void MeterModel::buildLadderRows() {
    ladderRows_.clear();
    if (!stream_) return;
    // Operative source list per MOX state.  Hard-coded sensible defaults
    // for first ship; per-row operator picker is a follow-on task.
    const bool tx = stream_->moxActive();
    QList<int> srcs;
    QStringList labels;
    if (tx) {
        // TX: full telemetry stack.  ALC / MIC / COMP get appended as
        // the TX audio chain lands at v0.2.x.
        srcs   = {PWR, SWR, PA_CURRENT, PA_VOLTS, TEMP};
        labels = {QStringLiteral("PWR"), QStringLiteral("SWR"),
                  QStringLiteral("PA"),  QStringLiteral("VDD"),
                  QStringLiteral("T")};
    } else {
        // RX: three-row degraded view.  The S-meter row is the hero
        // (matches the operator's RX expectation); noise floor + SNR
        // surface the other useful RX numbers the Arc/Bar bury inside
        // a single face.
        srcs   = {RX_SMETER};
        labels = {QStringLiteral("S")};
    }

    for (int i = 0; i < srcs.size(); ++i) {
        double level = 0.0, danger = 1.0;
        ladderRowFor(srcs[i], &level, &danger);
        QVariantMap row;
        row.insert(QStringLiteral("label"), labels[i]);
        row.insert(QStringLiteral("value"),
                   srcs[i] == RX_SMETER ? text_
                                        : formatSecondaryText(srcs[i]));
        row.insert(QStringLiteral("level"),  level);
        row.insert(QStringLiteral("danger"), danger);
        ladderRows_.append(row);
    }

    if (!tx) {
        // RX-only extra rows: noise floor (position on the scale) + SNR
        // delta.  No formatSecondaryText path because these are RX
        // S-meter sub-quantities, not Source enum values.
        QVariantMap nf;
        nf.insert(QStringLiteral("label"), QStringLiteral("NF"));
        nf.insert(QStringLiteral("value"),
                  QStringLiteral("%1 dBm").arg(std::lround(noiseFloorDbm_)));
        nf.insert(QStringLiteral("level"),  noiseLevel_);
        nf.insert(QStringLiteral("danger"), 1.0);
        ladderRows_.append(nf);

        QVariantMap snr;
        snr.insert(QStringLiteral("label"), QStringLiteral("SNR"));
        snr.insert(QStringLiteral("value"), snrText_);
        // SNR level: 0..60 dB axis, danger at 0 dB (signal at floor).
        const double snrDb = std::max(0.0, dispDbm_ - noiseFloorDbm_);
        snr.insert(QStringLiteral("level"),  std::clamp(snrDb / 60.0, 0.0, 1.0));
        snr.insert(QStringLiteral("danger"), 0.0);
        ladderRows_.append(snr);
    }
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
    case SWR:         computeSwr();    return;
    case PA_CURRENT:  // future HL2-telemetry source
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
    // The second secondary readout (task #37) is a TX-only feature; RX
    // already shows dBm + SNR on the two small lines.  Clear it so a
    // stale TX-source value doesn't render under the S-meter on swap.
    secondary2Text_.clear();

    hist_.push_back(n);
    if (int(hist_.size()) > kHistory) hist_.pop_front();
    for (int i = 0; i < kHistory; ++i) history_[i] = hist_[i];

    text_ = sLabel(dispDbm_);
    dbmText_ = QStringLiteral("%1 dBm").arg(dispDbm_, 0, 'f', 0);

    if (style_ == 2) buildLadderRows();
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
    // Secondary digital readout (task #36): if the operator has picked
    // a secondary source AND it's distinct from the current primary,
    // render its formatted value into the snrText_ slot that the
    // renderers already display below the main needle.  No secondary
    // configured → leave snrText_ blank so the renderer hides the line.
    if (txSecondary_ >= 0 && Source(txSecondary_) != source_)
        snrText_ = formatSecondaryText(txSecondary_);
    else
        snrText_.clear();
    // Second secondary readout (task #37): same idea, fills the third
    // text line so TX matches RX's primary/dBm/SNR three-line layout.
    // Suppressed when it equals the primary OR equals the first
    // secondary (no duplicate lines — Settings prevents the selection
    // but the model gates defensively).
    if (txSecondary2_ >= 0
        && Source(txSecondary2_) != source_
        && txSecondary2_ != txSecondary_)
        secondary2Text_ = formatSecondaryText(txSecondary2_);
    else
        secondary2Text_.clear();

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

    if (style_ == 2) buildLadderRows();
    emit updated();
}

void MeterModel::computeSwr() {
    // SWR — antenna match readout.  rho = sqrt(rev / fwd); SWR = (1+rho)
    // / (1-rho).  Real-world quirks the model has to handle:
    //
    //   * Below kSwrGuardW forward power the ADC noise floor dominates
    //     both fwd and rev — rho computes as a near-random number and
    //     SWR shows e.g. 8:1 on a perfectly matched dummy load.  Every
    //     commercial rig hides the readout in this regime; we mirror
    //     that ("—" text + zero level so the renderer shows a blank
    //     scale instead of a phantom needle).
    //
    //   * rev > fwd is physically impossible but the ADCs can flip
    //     briefly during the keydown TR-relay transient.  We clamp rho
    //     so SWR doesn't divide by zero or go negative.
    //
    //   * SWR above kSwrScaleMax pegs the meter; the text shows "≥N:1"
    //     so the operator knows it's off-scale rather than reading the
    //     pegged level as 3:1.
    const double fwd = stream_ ? stream_->fwdPowerW()
                                 : std::numeric_limits<double>::quiet_NaN();
    const double rev = stream_ ? stream_->revPowerW()
                                 : std::numeric_limits<double>::quiet_NaN();
    const bool lowPwr = std::isnan(fwd) || std::isnan(rev) ||
                         fwd < kSwrGuardW;

    double swr = kSwrScaleMin;   // perfect-match default for the smoothing
    if (!lowPwr) {
        const double r = std::clamp(rev / std::max(fwd, 1e-9), 0.0, 0.999);
        const double rho = std::sqrt(r);
        swr = std::clamp((1.0 + rho) / std::max(1.0 - rho, 1e-9),
                          kSwrScaleMin, 20.0);  // hard cap before pegging
    }

    // dispDbm_ is the source-agnostic smoothed-value cache; here it
    // holds smoothed SWR.  Initialize on first sample so the smoother
    // doesn't ramp up from -140 (S-meter init value) — that would
    // render as a long sweep from the left end on the first tick after
    // a swap from SMETER → SWR.
    if (dispDbm_ < kSwrScaleMin || dispDbm_ > 100.0) dispDbm_ = swr;
    dispDbm_ += kSwrSmooth * (swr - dispDbm_);

    if (lowPwr) {
        // No usable SWR — render zero state, blank text.  The model
        // still keeps its smoothing primed (so the first valid sample
        // doesn't lurch the meter), but the operator sees "—" instead
        // of bogus values.
        level_ = 0.0;
        peak_ = std::max(0.0, peak_ - kSwrPeakDecay);
        if (maxPeakEnabled_)
            maxPeak_ = std::max(0.0, maxPeak_ - kPwrMaxDecay);
        else
            maxPeak_ = 0.0;
        glow_ = std::max(0.0, glow_ - kSwrGlowDecay);
        text_ = QStringLiteral("—");
    } else {
        const double n = std::clamp(
            (dispDbm_ - kSwrScaleMin) / (kSwrScaleMax - kSwrScaleMin),
            0.0, 1.0);
        level_ = n;
        if (n >= peak_) { peak_ = n; holdCtr_ = peakHoldTicks_; }
        else if (holdCtr_ > 0) { --holdCtr_; }
        else { peak_ = std::max(0.0, peak_ - kSwrPeakDecay); }
        if (maxPeakEnabled_) {
            if (n >= maxPeak_) { maxPeak_ = n; maxHoldCtr_ = maxHoldTicks_; }
            else if (maxHoldCtr_ > 0) { --maxHoldCtr_; }
            else { maxPeak_ = std::max(0.0, maxPeak_ - kPwrMaxDecay); }
        } else {
            maxPeak_ = 0.0;
        }
        glow_ = (n >= glow_) ? n : std::max(n, glow_ - kSwrGlowDecay);
        // Pegged-above-scale gets the ≥-prefix so operators don't read
        // a stuck peg as the actual ratio.
        text_ = (dispDbm_ >= kSwrScaleMax)
                    ? QStringLiteral("≥%1:1").arg(kSwrScaleMax, 0, 'f', 1)
                    : QStringLiteral("%1:1").arg(dispDbm_, 0, 'f', 2);
    }

    // Danger zone at the standard 2.0:1 threshold (sits at 0.5 on the
    // 1.0..3.0 axis = half-scale, matching the PWR red-zone convention).
    normDanger_ = std::clamp(
        (kSwrDanger - kSwrScaleMin) / (kSwrScaleMax - kSwrScaleMin),
        0.0, 1.0);
    noiseLevel_ = 0.0;
    // Secondary digital readout (task #36) — same handling as PWR's
    // compute: render the operator's chosen second source into the
    // snrText_ slot the renderers display below the main needle.
    if (txSecondary_ >= 0 && Source(txSecondary_) != source_)
        snrText_ = formatSecondaryText(txSecondary_);
    else
        snrText_.clear();
    // Second secondary readout (task #37) — fills the third text line
    // so TX matches RX's three-line layout.  Hidden when it equals the
    // primary or duplicates the first secondary.
    if (txSecondary2_ >= 0
        && Source(txSecondary2_) != source_
        && txSecondary2_ != txSecondary_)
        secondary2Text_ = formatSecondaryText(txSecondary2_);
    else
        secondary2Text_.clear();
    dbmText_.clear();

    hist_.push_back(level_);
    if (int(hist_.size()) > kHistory) hist_.pop_front();
    for (int i = 0; i < kHistory; ++i) history_[i] = hist_[i];

    if (style_ == 2) buildLadderRows();
    emit updated();
}

} // namespace lyra::ui
