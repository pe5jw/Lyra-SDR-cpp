// Lyra — shared UI preferences.  See prefs.h.

#include "prefs.h"

#include "palettes.h"
#include "grid.h"

#include <QSettings>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lyra::ui {

namespace {
constexpr auto kGrid   = "panadapter/gridLevel";
constexpr auto kFps    = "panadapter/targetFps";
constexpr auto kDbMin  = "panadapter/dbMin";
constexpr auto kDbMax  = "panadapter/dbMax";
constexpr auto kDbAuto = "panadapter/dbAuto";
constexpr auto kTrace  = "panadapter/traceColor";
constexpr auto kMode   = "panadapter/traceMode";
constexpr auto kPal    = "panadapter/tracePalette";
constexpr auto kStr    = "panadapter/strengthColor";
constexpr auto kFill   = "panadapter/fillEnabled";
constexpr auto kFillCol = "panadapter/fillColor";
constexpr auto kSmooth = "panadapter/smoothing";
constexpr auto kGlow   = "panadapter/glow";
constexpr auto kSheen  = "panadapter/glassSheen";
constexpr auto kPkEn    = "panadapter/peakEnabled";
constexpr auto kPkHold  = "panadapter/peakHoldSecs";
constexpr auto kPkDecay = "panadapter/peakDecayDbps";
constexpr auto kPkStyle = "panadapter/peakStyle";
constexpr auto kPkColor = "panadapter/peakColor";
constexpr auto kPkShow  = "panadapter/peakShowDb";
constexpr auto kNfEn    = "panadapter/noiseFloorEnabled";
constexpr auto kNfColor = "panadapter/noiseFloorColor";
constexpr auto kWmark  = "panadapter/watermark";
constexpr auto kMet    = "panadapter/meteors";
constexpr auto kMetGap = "panadapter/meteorGap";
constexpr auto kMetGold = "panadapter/meteorGold";
constexpr auto kWfPal  = "panadapter/waterfallPalette";
constexpr auto kWfCol  = "panadapter/waterfallColor";
constexpr auto kWfSpd  = "panadapter/waterfallSpeed";
constexpr auto kWfDbMin = "panadapter/waterfallDbMin";
constexpr auto kWfDbMax = "panadapter/waterfallDbMax";
constexpr auto kWfDbAuto = "panadapter/waterfallDbAuto";
constexpr auto kPanSplit = "ui/panadapterSplit";
constexpr auto kCursorRdt = "panadapter/cursorReadout";
constexpr auto kZoom   = "panadapter/zoom";
constexpr auto kRxMode = "modefilter/mode";
constexpr auto kBwPrefix = "modefilter/bw/";   // + <MODE>
constexpr auto kWfCollapse = "panadapter/waterfallCollapsed";
constexpr auto kSampRate = "radio/sampleRate";
constexpr auto kOpCall  = "operator/callsign";
constexpr auto kOpGrid  = "operator/grid";
constexpr auto kOpLat   = "operator/lat_manual";
constexpr auto kOpLon   = "operator/lon_manual";
constexpr auto kBandRegion = "band_plan/region";
constexpr auto kBpSegs     = "band_plan/segments";
constexpr auto kBpLand     = "band_plan/landmarks";
constexpr auto kBpBeacons  = "band_plan/beacons";
constexpr auto kBpEdges    = "band_plan/edges";
constexpr auto kBpColorPfx = "band_plan/color_";   // + <kind>
constexpr auto kDebugLog   = "debug/logging";
// Segment-kind default colours (mirror band_plan.py SEGMENT_COLORS).
const QHash<QString, QString> kBpDefaultColors = {
    {QStringLiteral("CW"),  QStringLiteral("#3c5a9c")},
    {QStringLiteral("DIG"), QStringLiteral("#9c3c9c")},
    {QStringLiteral("SSB"), QStringLiteral("#3c9c6a")},
    {QStringLiteral("FM"),  QStringLiteral("#c47a2a")},
    {QStringLiteral("MIX"), QStringLiteral("#5c8caa")},
    {QStringLiteral("BC"),  QStringLiteral("#7a7a3a")},
};
const QStringList kRegions = {
    QStringLiteral("US"), QStringLiteral("IARU_R1"),
    QStringLiteral("IARU_R3"), QStringLiteral("NONE"),
};
// Modes whose per-mode bandwidth we persist/restore.
const QStringList kModes = {
    QStringLiteral("LSB"),  QStringLiteral("USB"),
    QStringLiteral("CWL"),  QStringLiteral("CWU"),
    QStringLiteral("DSB"),  QStringLiteral("AM"),
    QStringLiteral("FM"),   QStringLiteral("DIGU"),
    QStringLiteral("DIGL"),
};
} // namespace

Prefs::Prefs(QObject *parent) : QObject(parent) {
    QSettings s;
    gridLevel_  = std::clamp(s.value(kGrid, 35).toInt(), 0, 100);
    targetFps_  = std::clamp(s.value(kFps, 60).toInt(), 1, 240);
    dbMin_      = s.value(kDbMin, -130.0).toDouble();
    dbMax_      = s.value(kDbMax, -20.0).toDouble();
    dbAuto_     = s.value(kDbAuto, false).toBool();
    traceMode_  = std::clamp(s.value(kMode, 0).toInt(), 0, 1);
    traceColor_ = s.value(kTrace, QStringLiteral("#5ec8ff")).toString();
    palette_    = std::max(0, s.value(kPal, 0).toInt());
    strengthColor_ = s.value(kStr, QStringLiteral("#ff9b30")).toString();
    fillEnabled_ = s.value(kFill, true).toBool();
    fillColor_   = s.value(kFillCol, QStringLiteral("#5ec8ff")).toString();
    smoothing_   = std::clamp(s.value(kSmooth, 0).toInt(), 0, 10);
    glow_        = std::clamp(s.value(kGlow, 40).toInt(), 0, 100);
    glassSheen_  = std::clamp(s.value(kSheen, 20).toInt(), 0, 100);
    peakEnabled_   = s.value(kPkEn, false).toBool();
    peakHoldSecs_  = s.value(kPkHold, 0.0).toDouble();   // 0 = Off
    peakDecayDbps_ = std::clamp(s.value(kPkDecay, 10.0).toDouble(), 1.0, 120.0);
    peakStyle_     = std::clamp(s.value(kPkStyle, 1).toInt(), 0, 2);  // 1 dots
    peakColor_     = s.value(kPkColor, QStringLiteral("#ffbe5a")).toString();
    peakShowDb_    = s.value(kPkShow, false).toBool();
    noiseFloorEnabled_ = s.value(kNfEn, true).toBool();
    noiseFloorColor_   = s.value(kNfColor, QStringLiteral("#78c88c")).toString();
    watermark_   = s.value(kWmark, true).toBool();
    meteors_     = s.value(kMet, true).toBool();
    meteorGap_   = std::clamp(s.value(kMetGap, 30).toInt(), 5, 120);
    meteorGold_  = std::clamp(s.value(kMetGold, 15).toInt(), 0, 100);
    waterfallPalette_ = std::max(0, s.value(kWfPal, 0).toInt());
    waterfallColor_   = s.value(kWfCol, QStringLiteral("#30b0ff")).toString();
    waterfallSpeed_   = std::clamp(s.value(kWfSpd, 20).toInt(), 1, 120);
    waterfallDbMin_   = s.value(kWfDbMin, -120.0).toDouble();
    waterfallDbMax_   = s.value(kWfDbMax,  -20.0).toDouble();
    waterfallDbAuto_  = s.value(kWfDbAuto, false).toBool();
    panadapterSplit_  = s.value(kPanSplit);   // invalid (= QML undefined) if unset
    cursorReadout_    = s.value(kCursorRdt, true).toBool();
    zoom_             = std::clamp(s.value(kZoom, 1.0).toDouble(), 1.0, 32.0);
    mode_             = s.value(kRxMode, QStringLiteral("USB")).toString();
    // Per-mode RX bandwidth: load any persisted value; modes without a
    // stored value fall back to defaultBandwidthFor() on read.
    for (const QString &m : kModes) {
        const QVariant v = s.value(QString(kBwPrefix) + m);
        if (v.isValid()) {
            bwByMode_.insert(m, v.toInt());
        }
    }
    waterfallCollapsed_ = s.value(kWfCollapse, false).toBool();
    callsign_   = s.value(kOpCall, QString()).toString();
    gridSquare_ = lyra::ham::normalizeGrid(s.value(kOpGrid).toString());
    {
        constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
        const QVariant la = s.value(kOpLat), lo = s.value(kOpLon);
        manualLat_ = la.isValid() ? la.toDouble() : kNaN;
        manualLon_ = lo.isValid() ? lo.toDouble() : kNaN;
    }
    bandPlanRegion_ = s.value(kBandRegion, QStringLiteral("US")).toString();
    if (!kRegions.contains(bandPlanRegion_)) bandPlanRegion_ = QStringLiteral("US");
    bandPlanSegments_  = s.value(kBpSegs, true).toBool();
    bandPlanLandmarks_ = s.value(kBpLand, true).toBool();
    bandPlanBeacons_   = s.value(kBpBeacons, true).toBool();
    bandPlanEdges_     = s.value(kBpEdges, true).toBool();
    for (auto it = kBpDefaultColors.cbegin(); it != kBpDefaultColors.cend(); ++it) {
        const QVariant v = s.value(QString(kBpColorPfx) + it.key());
        if (v.isValid() && !v.toString().isEmpty())
            bandPlanColors_.insert(it.key(), v.toString());
    }
    debugLogging_ = s.value(kDebugLog, false).toBool();
    sampleRate_ = s.value(kSampRate, 192000).toInt();
    if (sampleRate_ != 96000 && sampleRate_ != 192000 && sampleRate_ != 384000)
        sampleRate_ = 192000;
}

int Prefs::defaultBandwidthFor(const QString &mode) {
    if (mode == QStringLiteral("CWL") || mode == QStringLiteral("CWU"))
        return 250;
    if (mode == QStringLiteral("DSB")) return 5000;
    if (mode == QStringLiteral("AM"))  return 6000;
    if (mode == QStringLiteral("FM"))  return 10000;
    if (mode == QStringLiteral("DIGL") || mode == QStringLiteral("DIGU"))
        return 3000;
    return 2400;   // SSB (USB/LSB) + anything else
}

QStringList Prefs::paletteNames() const {
    QStringList names = lyra::palettes::names();
    names << tr("Custom color…");   // index == preset count
    return names;
}

void Prefs::setGridLevel(int v) {
    v = std::clamp(v, 0, 100);
    if (v != gridLevel_) {
        gridLevel_ = v;
        QSettings().setValue(kGrid, v);
        emit gridLevelChanged();
    }
}

void Prefs::setTargetFps(int v) {
    v = std::clamp(v, 1, 240);
    if (v != targetFps_) {
        targetFps_ = v;
        QSettings().setValue(kFps, v);
        emit targetFpsChanged();
    }
}

void Prefs::setDbMin(double v) {
    if (v != dbMin_) {
        dbMin_ = v;
        QSettings().setValue(kDbMin, v);
        emit dbMinChanged();
    }
}

void Prefs::setDbMax(double v) {
    if (v != dbMax_) {
        dbMax_ = v;
        QSettings().setValue(kDbMax, v);
        emit dbMaxChanged();
    }
}

void Prefs::setDbAuto(bool v) {
    if (v != dbAuto_) {
        dbAuto_ = v;
        QSettings().setValue(kDbAuto, v);
        emit dbAutoChanged();
    }
}

void Prefs::setTraceMode(int v) {
    v = std::clamp(v, 0, 1);
    if (v != traceMode_) {
        traceMode_ = v;
        QSettings().setValue(kMode, v);
        emit traceModeChanged();
    }
}

void Prefs::setTraceColor(const QString &hex) {
    if (hex != traceColor_ && !hex.isEmpty()) {
        traceColor_ = hex;
        QSettings().setValue(kTrace, hex);
        emit traceColorChanged();
    }
}

void Prefs::setPalette(int v) {
    if (v < 0) v = 0;
    if (v != palette_) {
        palette_ = v;
        QSettings().setValue(kPal, v);
        emit paletteChanged();
    }
}

void Prefs::setStrengthColor(const QString &hex) {
    if (hex != strengthColor_ && !hex.isEmpty()) {
        strengthColor_ = hex;
        QSettings().setValue(kStr, hex);
        emit strengthColorChanged();
    }
}

void Prefs::setFillEnabled(bool v) {
    if (v != fillEnabled_) {
        fillEnabled_ = v;
        QSettings().setValue(kFill, v);
        emit fillEnabledChanged();
    }
}

void Prefs::setFillColor(const QString &hex) {
    if (hex != fillColor_ && !hex.isEmpty()) {
        fillColor_ = hex;
        QSettings().setValue(kFillCol, hex);
        emit fillColorChanged();
    }
}

void Prefs::setSmoothing(int v) {
    v = std::clamp(v, 0, 10);
    if (v != smoothing_) {
        smoothing_ = v;
        QSettings().setValue(kSmooth, v);
        emit smoothingChanged();
    }
}

void Prefs::setGlow(int v) {
    v = std::clamp(v, 0, 100);
    if (v != glow_) {
        glow_ = v;
        QSettings().setValue(kGlow, v);
        emit glowChanged();
    }
}

void Prefs::setGlassSheen(int v) {
    v = std::clamp(v, 0, 100);
    if (v != glassSheen_) {
        glassSheen_ = v;
        QSettings().setValue(kSheen, v);
        emit glassSheenChanged();
    }
}

void Prefs::setPeakEnabled(bool v) {
    if (v != peakEnabled_) {
        peakEnabled_ = v;
        QSettings().setValue(kPkEn, v);
        emit peakEnabledChanged();
    }
}

void Prefs::setPeakHoldSecs(double v) {
    if (v != peakHoldSecs_) {
        peakHoldSecs_ = v;
        QSettings().setValue(kPkHold, v);
        emit peakHoldSecsChanged();
    }
}

void Prefs::setPeakDecayDbps(double v) {
    v = std::clamp(v, 1.0, 120.0);
    if (v != peakDecayDbps_) {
        peakDecayDbps_ = v;
        QSettings().setValue(kPkDecay, v);
        emit peakDecayDbpsChanged();
    }
}

void Prefs::setPeakStyle(int v) {
    v = std::clamp(v, 0, 2);
    if (v != peakStyle_) {
        peakStyle_ = v;
        QSettings().setValue(kPkStyle, v);
        emit peakStyleChanged();
    }
}

void Prefs::setPeakColor(const QString &hex) {
    if (hex != peakColor_ && !hex.isEmpty()) {
        peakColor_ = hex;
        QSettings().setValue(kPkColor, hex);
        emit peakColorChanged();
    }
}

void Prefs::setPeakShowDb(bool v) {
    if (v != peakShowDb_) {
        peakShowDb_ = v;
        QSettings().setValue(kPkShow, v);
        emit peakShowDbChanged();
    }
}

void Prefs::setNoiseFloorEnabled(bool v) {
    if (v != noiseFloorEnabled_) {
        noiseFloorEnabled_ = v;
        QSettings().setValue(kNfEn, v);
        emit noiseFloorEnabledChanged();
    }
}

void Prefs::setNoiseFloorColor(const QString &hex) {
    if (hex != noiseFloorColor_ && !hex.isEmpty()) {
        noiseFloorColor_ = hex;
        QSettings().setValue(kNfColor, hex);
        emit noiseFloorColorChanged();
    }
}

void Prefs::setWatermark(bool v) {
    if (v != watermark_) {
        watermark_ = v;
        QSettings().setValue(kWmark, v);
        emit watermarkChanged();
    }
}

void Prefs::setMeteors(bool v) {
    if (v != meteors_) {
        meteors_ = v;
        QSettings().setValue(kMet, v);
        emit meteorsChanged();
    }
}

void Prefs::setMeteorGap(int v) {
    v = std::clamp(v, 5, 120);
    if (v != meteorGap_) {
        meteorGap_ = v;
        QSettings().setValue(kMetGap, v);
        emit meteorGapChanged();
    }
}

void Prefs::setMeteorGold(int v) {
    v = std::clamp(v, 0, 100);
    if (v != meteorGold_) {
        meteorGold_ = v;
        QSettings().setValue(kMetGold, v);
        emit meteorGoldChanged();
    }
}

void Prefs::setWaterfallPalette(int v) {
    if (v < 0) v = 0;
    if (v != waterfallPalette_) {
        waterfallPalette_ = v;
        QSettings().setValue(kWfPal, v);
        emit waterfallPaletteChanged();
    }
}

void Prefs::setWaterfallColor(const QString &hex) {
    if (hex != waterfallColor_ && !hex.isEmpty()) {
        waterfallColor_ = hex;
        QSettings().setValue(kWfCol, hex);
        emit waterfallColorChanged();
    }
}

void Prefs::setWaterfallSpeed(int v) {
    v = std::clamp(v, 1, 120);
    if (v != waterfallSpeed_) {
        waterfallSpeed_ = v;
        QSettings().setValue(kWfSpd, v);
        emit waterfallSpeedChanged();
    }
}

void Prefs::setWaterfallDbMin(double v) {
    if (v != waterfallDbMin_) {
        waterfallDbMin_ = v;
        QSettings().setValue(kWfDbMin, v);
        emit waterfallDbMinChanged();
    }
}

void Prefs::setWaterfallDbMax(double v) {
    if (v != waterfallDbMax_) {
        waterfallDbMax_ = v;
        QSettings().setValue(kWfDbMax, v);
        emit waterfallDbMaxChanged();
    }
}

void Prefs::setWaterfallDbAuto(bool v) {
    if (v != waterfallDbAuto_) {
        waterfallDbAuto_ = v;
        QSettings().setValue(kWfDbAuto, v);
        emit waterfallDbAutoChanged();
    }
}

void Prefs::setCursorReadout(bool v) {
    if (v != cursorReadout_) {
        cursorReadout_ = v;
        QSettings().setValue(kCursorRdt, v);
        emit cursorReadoutChanged();
    }
}

void Prefs::setZoom(double v) {
    v = std::clamp(v, 1.0, 32.0);
    if (v != zoom_) {
        zoom_ = v;
        QSettings().setValue(kZoom, v);
        emit zoomChanged();
    }
}

int Prefs::rxBandwidth() const {
    return bwByMode_.value(mode_, defaultBandwidthFor(mode_));
}

void Prefs::setMode(const QString &m) {
    if (m.isEmpty() || m == mode_) {
        return;
    }
    mode_ = m;
    QSettings().setValue(kRxMode, m);
    emit modeChanged();
    // The effective bandwidth is per-mode, so it changes with the mode.
    emit rxBandwidthChanged();
}

void Prefs::setRxBandwidth(int hz) {
    if (hz <= 0 || hz == rxBandwidth()) {
        return;
    }
    bwByMode_.insert(mode_, hz);
    QSettings().setValue(QString(kBwPrefix) + mode_, hz);
    emit rxBandwidthChanged();
}

void Prefs::setSampleRate(int hz) {
    if (hz != 96000 && hz != 192000 && hz != 384000) {
        return;
    }
    if (hz != sampleRate_) {
        sampleRate_ = hz;
        QSettings().setValue(kSampRate, hz);
        emit sampleRateChanged();
    }
}

void Prefs::setWaterfallCollapsed(bool v) {
    if (v != waterfallCollapsed_) {
        waterfallCollapsed_ = v;
        QSettings().setValue(kWfCollapse, v);
        emit waterfallCollapsedChanged();
    }
}

void Prefs::setPanadapterSplit(const QVariant &v) {
    // Always persist + notify (even to an invalid QVariant — that's how
    // "Reset to default layout" tells QML to restore the factory split).
    panadapterSplit_ = v;
    if (v.isValid()) {
        QSettings().setValue(kPanSplit, v);
    } else {
        QSettings().remove(kPanSplit);
    }
    emit panadapterSplitChanged();
}

void Prefs::setCallsign(const QString &c) {
    const QString v = c.trimmed().toUpper();   // no format validation (old Lyra)
    if (v != callsign_) {
        callsign_ = v;
        QSettings().setValue(kOpCall, v);
        emit callsignChanged();
    }
}

void Prefs::setGridSquare(const QString &g) {
    const QString v = lyra::ham::normalizeGrid(g);   // "" if invalid
    if (v != gridSquare_) {
        gridSquare_ = v;
        QSettings().setValue(kOpGrid, v);
        emit gridSquareChanged();
        emit locationChanged();   // effective lat/lon follows the grid
    }
}

void Prefs::setManualLatLon(double lat, double lon) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    QSettings s;
    if (std::isnan(lat) || std::isnan(lon)) {        // clear the override
        const bool had = !std::isnan(manualLat_) || !std::isnan(manualLon_);
        manualLat_ = kNaN;
        manualLon_ = kNaN;
        s.remove(kOpLat);
        s.remove(kOpLon);
        if (had) emit locationChanged();
        return;
    }
    lat = std::clamp(lat, -90.0, 90.0);
    lon = std::clamp(lon, -180.0, 180.0);
    if (lat != manualLat_ || lon != manualLon_) {
        manualLat_ = lat;
        manualLon_ = lon;
        s.setValue(kOpLat, lat);
        s.setValue(kOpLon, lon);
        emit locationChanged();
    }
}

void Prefs::setBandPlanRegion(const QString &r) {
    const QString v = kRegions.contains(r) ? r : QStringLiteral("US");
    if (v != bandPlanRegion_) {
        bandPlanRegion_ = v;
        QSettings().setValue(kBandRegion, v);
        emit bandPlanRegionChanged();
    }
}

void Prefs::setBandPlanSegments(bool v) {
    if (v != bandPlanSegments_) {
        bandPlanSegments_ = v;
        QSettings().setValue(kBpSegs, v);
        emit bandPlanSegmentsChanged();
    }
}

void Prefs::setBandPlanLandmarks(bool v) {
    if (v != bandPlanLandmarks_) {
        bandPlanLandmarks_ = v;
        QSettings().setValue(kBpLand, v);
        emit bandPlanLandmarksChanged();
    }
}

void Prefs::setBandPlanBeacons(bool v) {
    if (v != bandPlanBeacons_) {
        bandPlanBeacons_ = v;
        QSettings().setValue(kBpBeacons, v);
        emit bandPlanBeaconsChanged();
    }
}

void Prefs::setBandPlanEdges(bool v) {
    if (v != bandPlanEdges_) {
        bandPlanEdges_ = v;
        QSettings().setValue(kBpEdges, v);
        emit bandPlanEdgesChanged();
    }
}

QString Prefs::defaultBandPlanColor(const QString &kind) {
    return kBpDefaultColors.value(kind, QStringLiteral("#5c8caa"));
}

QString Prefs::bandPlanColor(const QString &kind) const {
    return bandPlanColors_.value(kind, defaultBandPlanColor(kind));
}

void Prefs::setBandPlanColor(const QString &kind, const QString &hex) {
    const QString def = defaultBandPlanColor(kind);
    QSettings s;
    if (hex.isEmpty() || hex.compare(def, Qt::CaseInsensitive) == 0) {
        // Back to default → drop the override.
        if (bandPlanColors_.remove(kind) > 0) {
            s.remove(QString(kBpColorPfx) + kind);
            emit bandPlanColorsChanged();
        }
        return;
    }
    if (bandPlanColors_.value(kind) != hex) {
        bandPlanColors_.insert(kind, hex);
        s.setValue(QString(kBpColorPfx) + kind, hex);
        emit bandPlanColorsChanged();
    }
}

void Prefs::setDebugLogging(bool on) {
    if (on != debugLogging_) {
        debugLogging_ = on;
        QSettings().setValue(kDebugLog, on);
        emit debugLoggingChanged();
    }
}

bool Prefs::operatorLocation(double *lat, double *lon) const {
    if (const auto ll = lyra::ham::gridToLatLon(gridSquare_)) {
        if (lat) *lat = ll->first;
        if (lon) *lon = ll->second;
        return true;
    }
    if (!std::isnan(manualLat_) && !std::isnan(manualLon_)) {
        if (lat) *lat = manualLat_;
        if (lon) *lon = manualLon_;
        return true;
    }
    return false;
}

} // namespace lyra::ui
