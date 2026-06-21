// Lyra — amateur band-plan data.  See bandplan.h.
//
// Faithful port of old Lyra's band_plan.py: same regions, segment
// freq ranges, kind→colour palette, and the shared landmark list.

#include "bandplan.h"

#include "prefs.h"

#include <QDateTime>
#include <QVariantMap>
#include <QtMath>

namespace lyra::ui {

namespace {

struct Seg  { qint64 lo; qint64 hi; const char *kind; const char *label; };
struct Band { const char *name; qint64 lo; qint64 hi; QVector<Seg> segs; };
struct Mark { qint64 freq; const char *label; const char *mode; bool beacon; };

// Segment kind → colour (base #RRGGBB; QML applies the strip alpha).
QString kindColor(const QString &k) {
    if (k == QLatin1String("CW"))  return QStringLiteral("#3c5a9c"); // deep blue
    if (k == QLatin1String("DIG")) return QStringLiteral("#9c3c9c"); // magenta
    if (k == QLatin1String("SSB")) return QStringLiteral("#3c9c6a"); // green
    if (k == QLatin1String("FM"))  return QStringLiteral("#c47a2a"); // orange
    if (k == QLatin1String("MIX")) return QStringLiteral("#5c8caa"); // teal
    if (k == QLatin1String("BC"))  return QStringLiteral("#7a7a3a"); // olive
    return QStringLiteral("#5c8caa");
}

// ── US (FCC / IARU R2) ───────────────────────────────────────────────
const QVector<Band> &usBands() {
    static const QVector<Band> b = {
        {"160m", 1800000, 2000000, {
            {1800000, 1850000, "CW",  "CW/DIG"},
            {1850000, 2000000, "SSB", "SSB"}}},
        {"80m", 3500000, 4000000, {
            {3500000, 3600000, "CW",  "CW"},
            {3570000, 3600000, "DIG", "DIG"},
            {3600000, 4000000, "SSB", "SSB"}}},
        // US 60m: 5 fixed channels, USB only.  Each entry is
        // [suppressed-carrier dial, dial + 2.8 kHz] so the channel bar
        // spans the real 2.8 kHz channel and the click QSYs to the dial
        // (the operator's tuned readout in USB).  FCC centres are
        // dial + 1.5 kHz (5332.0 / 5348.0 / 5358.5 / 5373.0 / 5405.0).
        {"60m", 5330500, 5406300, {
            {5330500, 5333300, "SSB", "CH1"},
            {5346500, 5349300, "SSB", "CH2"},
            {5357000, 5359800, "SSB", "CH3"},
            {5371500, 5374300, "SSB", "CH4"},
            {5403500, 5406300, "SSB", "CH5"}}},
        {"40m", 7000000, 7300000, {
            {7000000, 7125000, "CW",  "CW"},
            {7040000, 7100000, "DIG", "DIG"},
            {7125000, 7300000, "SSB", "SSB"}}},
        {"30m", 10100000, 10150000, {
            {10100000, 10150000, "DIG", "CW/DIG"}}},
        {"20m", 14000000, 14350000, {
            {14000000, 14150000, "CW",  "CW"},
            {14070000, 14112000, "DIG", "DIG"},
            {14150000, 14350000, "SSB", "SSB"}}},
        {"17m", 18068000, 18168000, {
            {18068000, 18110000, "CW",  "CW/DIG"},
            {18110000, 18168000, "SSB", "SSB"}}},
        {"15m", 21000000, 21450000, {
            {21000000, 21200000, "CW",  "CW"},
            {21070000, 21110000, "DIG", "DIG"},
            {21200000, 21450000, "SSB", "SSB"}}},
        {"12m", 24890000, 24990000, {
            {24890000, 24930000, "CW",  "CW/DIG"},
            {24930000, 24990000, "SSB", "SSB"}}},
        {"10m", 28000000, 29700000, {
            {28000000, 28300000, "CW",  "CW"},
            {28070000, 28120000, "DIG", "DIG"},
            {28300000, 29500000, "SSB", "SSB"},
            {29500000, 29700000, "FM",  "FM"}}},
        {"6m", 50000000, 54000000, {
            {50000000, 50100000, "CW",  "CW"},
            {50100000, 50500000, "SSB", "SSB"},
            {50500000, 54000000, "FM",  "FM"}}},
    };
    return b;
}

// ── IARU Region 1 (Europe / Africa / Middle East) ────────────────────
const QVector<Band> &r1Bands() {
    static const QVector<Band> b = {
        {"160m", 1810000, 2000000, {
            {1810000, 1838000, "CW",  "CW"},
            {1838000, 1843000, "DIG", "DIG"},
            {1843000, 2000000, "SSB", "SSB"}}},
        {"80m", 3500000, 3800000, {
            {3500000, 3580000, "CW",  "CW"},
            {3580000, 3620000, "DIG", "DIG"},
            {3600000, 3800000, "SSB", "SSB"}}},
        // 60m: WRC-15 secondary allocation, 5351.5-5366.5 kHz, all-mode
        // (15 W EIRP).  Contiguous band, NOT channelized like the US.
        // IARU R1 internal usage: CW/narrow below 5354, all-mode above.
        // R3 reuses this table (bandsFor), so it inherits the same band.
        {"60m", 5351500, 5366500, {
            {5351500, 5354000, "CW",  "CW/DIG"},
            {5354000, 5366500, "SSB", "SSB"}}},
        {"40m", 7000000, 7200000, {
            {7000000, 7040000, "CW",  "CW"},
            {7040000, 7060000, "DIG", "DIG"},
            {7060000, 7200000, "SSB", "SSB"}}},
        {"30m", 10100000, 10150000, {
            {10100000, 10150000, "DIG", "CW/DIG"}}},
        {"20m", 14000000, 14350000, {
            {14000000, 14070000, "CW",  "CW"},
            {14070000, 14099000, "DIG", "DIG"},
            {14099000, 14350000, "SSB", "SSB"}}},
        {"17m", 18068000, 18168000, {
            {18068000, 18095000, "CW",  "CW"},
            {18095000, 18110000, "DIG", "DIG"},
            {18110000, 18168000, "SSB", "SSB"}}},
        {"15m", 21000000, 21450000, {
            {21000000, 21070000, "CW",  "CW"},
            {21070000, 21151000, "DIG", "DIG"},
            {21151000, 21450000, "SSB", "SSB"}}},
        {"12m", 24890000, 24990000, {
            {24890000, 24915000, "CW",  "CW"},
            {24915000, 24930000, "DIG", "DIG"},
            {24930000, 24990000, "SSB", "SSB"}}},
        {"10m", 28000000, 29700000, {
            {28000000, 28070000, "CW",  "CW"},
            {28070000, 28190000, "DIG", "DIG"},
            {28190000, 29520000, "SSB", "SSB"},
            {29520000, 29700000, "FM",  "FM"}}},
        {"6m", 50000000, 54000000, {
            {50000000, 50100000, "CW",  "CW"},
            {50100000, 50500000, "SSB", "SSB"},
            {50500000, 54000000, "FM",  "FM"}}},
    };
    return b;
}

// Shared landmarks (FT8 / FT4 / WSPR / PSK + NCDXF beacons).
const QVector<Mark> &commonLandmarks() {
    static const QVector<Mark> m = {
        {1840000,  "FT8",  "DIGU", false}, {3573000,  "FT8",  "DIGU", false},
        {7074000,  "FT8",  "DIGU", false}, {10136000, "FT8",  "DIGU", false},
        {14074000, "FT8",  "DIGU", false}, {18100000, "FT8",  "DIGU", false},
        {21074000, "FT8",  "DIGU", false}, {24915000, "FT8",  "DIGU", false},
        {28074000, "FT8",  "DIGU", false}, {50313000, "FT8",  "DIGU", false},
        {3575000,  "FT4",  "DIGU", false}, {7047500,  "FT4",  "DIGU", false},
        {10140000, "FT4",  "DIGU", false}, {14080000, "FT4",  "DIGU", false},
        {18104000, "FT4",  "DIGU", false}, {21140000, "FT4",  "DIGU", false},
        {28180000, "FT4",  "DIGU", false},
        {1838100,  "WSPR", "DIGU", false}, {3568600,  "WSPR", "DIGU", false},
        {7038600,  "WSPR", "DIGU", false}, {10138700, "WSPR", "DIGU", false},
        {14095600, "WSPR", "DIGU", false}, {18104600, "WSPR", "DIGU", false},
        {21094600, "WSPR", "DIGU", false}, {24924600, "WSPR", "DIGU", false},
        {28124600, "WSPR", "DIGU", false}, {50293000, "WSPR", "DIGU", false},
        {7070000,  "PSK",  "DIGU", false}, {14070000, "PSK",  "DIGU", false},
        {21070000, "PSK",  "DIGU", false}, {28120000, "PSK",  "DIGU", false},
        {14100000, "NCDXF", "CWU", true},  {18110000, "NCDXF", "CWU", true},
        {21150000, "NCDXF", "CWU", true},  {24930000, "NCDXF", "CWU", true},
        {28200000, "NCDXF", "CWU", true},
    };
    return m;
}

// Region base table (the IARU-region default).
const QVector<Band> &regionBands(const QString &region) {
    if (region == QLatin1String("IARU_R1") ||
        region == QLatin1String("IARU_R3"))   // R3 reuses R1 on HF
        return r1Bands();
    return usBands();   // US / default
}

// Country-specific band overrides, layered on top of the region base.
// Only countries whose allocation deviates from their IARU region need
// an entry; an override REPLACES the same-named band in the base table.
// Everyone else (country == "AUTO"/unknown) inherits the region table.
const QVector<Band> &countryBands(const QString &country) {
    static const QVector<Band> none;
    if (country == QLatin1String("UK")) {
        // UK 5 MHz (Ofcom / RSGB band plan): 11 discrete segments,
        // USB, 100 W TX / 200 W EIRP, Full licence only.  Contiguous
        // band it is NOT — these are the only permitted slices.
        static const QVector<Band> uk = {
            {"60m", 5258500, 5406500, {
                {5258500, 5264000, "MIX", ""},
                {5276000, 5284000, "MIX", ""},
                {5288500, 5292000, "MIX", ""},
                {5298000, 5307000, "MIX", ""},
                {5313000, 5323000, "MIX", ""},
                {5333000, 5338000, "MIX", ""},
                {5354000, 5358000, "MIX", ""},
                {5362000, 5374500, "MIX", ""},
                {5378000, 5382000, "MIX", ""},
                {5395000, 5401500, "MIX", ""},
                {5403500, 5406500, "MIX", ""}}},
        };
        return uk;
    }
    if (country == QLatin1String("CA")) {
        // Canada (ISED): the WRC-15 5351.5-5366.5 kHz band, same as
        // IARU R1 -- NOT the US 5-channel plan, even though Canada is
        // IARU Region 2 (the "US" base bucket).
        static const QVector<Band> ca = {
            {"60m", 5351500, 5366500, {
                {5351500, 5354000, "CW",  "CW/DIG"},
                {5354000, 5366500, "SSB", "SSB"}}},
        };
        return ca;
    }
    return none;
}

// Region base merged with any country override.  An override replaces a
// same-named band in place (else appends).  Returned by value: the
// tables are tiny and this is rebuilt only on an overlay re-query
// (region/country/center/span change), never per frame.
QVector<Band> bandsFor(const QString &region, const QString &country) {
    const QVector<Band> &base = regionBands(region);
    const QVector<Band> &ov   = countryBands(country);
    if (ov.isEmpty()) return base;
    QVector<Band> merged = base;
    for (const Band &o : ov) {
        bool replaced = false;
        for (Band &m : merged) {
            if (qstrcmp(m.name, o.name) == 0) { m = o; replaced = true; break; }
        }
        if (!replaced) merged.append(o);
    }
    return merged;
}

} // namespace

BandPlan::BandPlan(Prefs *prefs, QObject *parent)
    : QObject(parent), prefs_(prefs) {
    if (prefs_) {
        connect(prefs_, &Prefs::bandPlanRegionChanged,
                this, &BandPlan::regionChanged);
        // A segment-colour change also needs the overlay to re-query;
        // regionChanged is the QML re-query trigger, so reuse it.
        connect(prefs_, &Prefs::bandPlanColorsChanged,
                this, &BandPlan::regionChanged);
        // A country-override change is the same kind of re-query trigger.
        connect(prefs_, &Prefs::bandPlanCountryChanged,
                this, &BandPlan::regionChanged);
    }
}

QString BandPlan::region() const {
    return prefs_ ? prefs_->bandPlanRegion() : QStringLiteral("US");
}

QString BandPlan::country() const {
    return prefs_ ? prefs_->bandPlanCountry() : QStringLiteral("AUTO");
}

QVariantList BandPlan::segments(double centerHz, double spanHz) const {
    QVariantList out;
    const QString reg = region();
    if (spanHz <= 0 || reg == QLatin1String("NONE")) return out;
    const qint64 lo = qint64(centerHz - spanHz / 2);
    const qint64 hi = qint64(centerHz + spanHz / 2);
    for (const Band &b : bandsFor(reg, country())) {
        if (b.hi <= lo || b.lo >= hi) continue;
        for (const Seg &s : b.segs) {
            if (s.hi <= lo || s.lo >= hi) continue;
            QVariantMap m;
            const QString kind = QString::fromLatin1(s.kind);
            const QString label = QString::fromLatin1(s.label);
            // Channelized sub-bands (US 60m CH1..CH5) are narrow discrete
            // windows, not wide ranges — flag them so the overlay can draw
            // a bigger, click-to-QSY label like the watering-hole marks.
            const bool isChan = label.startsWith(QLatin1String("CH"));
            m["lo"]    = double(qMax<qint64>(s.lo, lo));
            m["hi"]    = double(qMin<qint64>(s.hi, hi));
            m["color"] = prefs_ ? prefs_->bandPlanColor(kind) : kindColor(kind);
            m["label"] = label;
            m["channel"] = isChan;
            // QSY target = the channel's suppressed-carrier dial freq =
            // its low edge (US 60m is USB; the dial reads the bottom of
            // the passband).  Use the unclamped edge, not the view-clamped
            // lo above.
            if (isChan)
                m["qsy"] = double(s.lo);
            out.append(m);
        }
    }
    return out;
}

QVariantList BandPlan::edges(double centerHz, double spanHz) const {
    QVariantList out;
    const QString reg = region();
    if (spanHz <= 0 || reg == QLatin1String("NONE")) return out;
    const qint64 lo = qint64(centerHz - spanHz / 2);
    const qint64 hi = qint64(centerHz + spanHz / 2);
    for (const Band &b : bandsFor(reg, country())) {
        if (b.lo > lo && b.lo < hi) {
            QVariantMap m; m["freq"] = double(b.lo);
            m["name"] = QString::fromLatin1(b.name); out.append(m);
        }
        if (b.hi > lo && b.hi < hi) {
            QVariantMap m; m["freq"] = double(b.hi);
            m["name"] = QString::fromLatin1(b.name); out.append(m);
        }
    }
    return out;
}

QString BandPlan::ncdxfStation(double freqHz) const {
    // 5 NCDXF beacon bands (kHz), in the canonical rotation order.
    static const int bandKhz[5] = {14100, 18110, 21150, 24930, 28200};
    const int fkhz = int(qRound(freqHz / 1000.0));
    int bandIdx = -1;
    for (int i = 0; i < 5; ++i)
        if (qAbs(fkhz - bandKhz[i]) <= 1) { bandIdx = i; break; }
    if (bandIdx < 0) return QString();   // not a beacon frequency

    // 18 stations, 10-second slots, 3-minute (180 s) cycle.  Slot 0
    // begins at second 0 of any UTC minute where minute % 3 == 0.
    const QTime t = QDateTime::currentDateTimeUtc().time();
    const int slot = ((t.minute() % 3) * 60 + t.second()) / 10;   // 0..17
    const int st = ((slot - bandIdx) % 18 + 18) % 18;

    static const char *calls[18] = {
        "4U1UN", "VE8AT", "W6WX", "KH6RS", "ZL6B", "VK6RBP", "JA2IGY",
        "RR9O", "VR2HK6", "4S7B", "ZS6DN", "5Z4B", "4X6TU", "OH2B",
        "CS3B", "LU4AA", "OA4B", "YV5B"};
    static const char *qth[18] = {
        "United Nations, NY", "Eureka, Canada", "Mt Umunhum, CA",
        "Maui, HI", "Masterton, NZ", "Perth, Australia", "Mt Asama, Japan",
        "Novosibirsk, Russia", "Hong Kong", "Colombo, Sri Lanka",
        "Pretoria, South Africa", "Kilimambogo, Kenya", "Tel Aviv, Israel",
        "Lohja, Finland", "Madeira, Portugal", "Buenos Aires, Argentina",
        "Lima, Peru", "Caracas, Venezuela"};
    return QStringLiteral("%1 — %2").arg(QString::fromLatin1(calls[st]),
                                         QString::fromLatin1(qth[st]));
}

QVariantList BandPlan::cbChannels(double centerHz, double spanHz) const {
    QVariantList out;
    if (spanHz <= 0 || !prefs_ || !prefs_->cbBandEnabled()) return out;
    // US 40-channel CB plan (channel number → kHz), incl. the 23/24/25
    // ordering anomaly.  A few channels carry their well-known nicknames.
    static const struct { int ch; int khz; const char *note; } chans[40] = {
        {1,26965,""},{2,26975,""},{3,26985,""},{4,27005,""},{5,27015,""},
        {6,27025,"Super Bowl"},{7,27035,""},{8,27055,""},
        {9,27065,"Emergency"},{10,27075,""},{11,27085,"Calling"},
        {12,27105,""},{13,27115,""},{14,27125,""},{15,27135,""},{16,27155,""},
        {17,27165,""},{18,27175,""},{19,27185,"Highway / truckers"},
        {20,27205,""},{21,27215,""},{22,27225,""},{23,27255,""},{24,27235,""},
        {25,27245,""},{26,27265,""},{27,27275,""},{28,27285,""},{29,27295,""},
        {30,27305,""},{31,27315,""},{32,27325,""},{33,27335,""},{34,27345,""},
        {35,27355,""},{36,27365,""},{37,27375,""},{38,27385,"SSB LSB"},
        {39,27395,""},{40,27405,"SSB"}};
    const double lo = centerHz - spanHz / 2;
    const double hi = centerHz + spanHz / 2;
    for (const auto &c : chans) {
        const double f = double(c.khz) * 1000.0;
        if (f < lo || f > hi) continue;
        QVariantMap m;
        m["freq"]  = f;
        m["label"] = QStringLiteral("Ch%1").arg(c.ch);
        m["note"]  = QString::fromLatin1(c.note);
        out.append(m);
    }
    return out;
}

QString BandPlan::bandContaining(double freqHz) const {
    const QString reg = region();
    if (reg == QLatin1String("NONE")) return QString();
    const qint64 f = qint64(freqHz);
    for (const Band &b : bandsFor(reg, country()))
        if (f >= b.lo && f < b.hi) return QString::fromLatin1(b.name);
    return QString();
}

QVariantList BandPlan::landmarks(double centerHz, double spanHz,
                                 bool showDigital, bool showBeacons) const {
    QVariantList out;
    const QString reg = region();
    if (spanHz <= 0 || reg == QLatin1String("NONE")) return out;
    const double lo = centerHz - spanHz / 2;
    const double hi = centerHz + spanHz / 2;
    for (const Mark &m : commonLandmarks()) {
        if (m.freq < lo || m.freq > hi) continue;
        if (m.beacon && !showBeacons) continue;
        if (!m.beacon && !showDigital) continue;
        QVariantMap v;
        v["freq"]   = double(m.freq);
        v["label"]  = QString::fromLatin1(m.label);
        v["mode"]   = QString::fromLatin1(m.mode);
        v["beacon"] = m.beacon;
        out.append(v);
    }
    return out;
}

} // namespace lyra::ui
