// Lyra — Maidenhead grid helpers.  See grid.h.  Faithful port of old
// Lyra lyra/ham/grid.py (grid_to_latlon returns cell-centre lat/lon).

#include "grid.h"

#include <QRegularExpression>

namespace lyra::ham {

namespace {
const QRegularExpression &gridRe() {
    static const QRegularExpression re(
        QStringLiteral("^[A-R]{2}[0-9]{2}([A-X]{2}([0-9]{2})?)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}
} // namespace

bool isValidGrid(const QString &grid) {
    if (grid.isEmpty()) return false;
    return gridRe().match(grid.trimmed()).hasMatch();
}

QString normalizeGrid(const QString &grid) {
    return isValidGrid(grid) ? grid.trimmed().toUpper() : QString();
}

std::optional<std::pair<double, double>> gridToLatLon(const QString &grid) {
    if (!isValidGrid(grid)) return std::nullopt;
    const QString g = grid.trimmed().toUpper();
    const auto C = [&g](int i) { return g.at(i).unicode(); };

    // Field pair — 18×18 cells of 20° lon × 10° lat.
    double lon = (C(0) - 'A') * 20.0 - 180.0;
    double lat = (C(1) - 'A') * 10.0 - 90.0;
    // Square pair — 10×10 of 2° lon × 1° lat.
    lon += (C(2) - '0') * 2.0;
    lat += (C(3) - '0') * 1.0;

    if (g.size() == 4) {                       // centre of the square cell
        lon += 1.0;
        lat += 0.5;
        return std::make_pair(lat, lon);
    }

    // Subsquare pair — 24×24 of 5' lon × 2.5' lat.
    lon += (C(4) - 'A') * (5.0 / 60.0);
    lat += (C(5) - 'A') * (2.5 / 60.0);

    if (g.size() == 6) {                       // + half a subsquare
        lon += (5.0 / 60.0) / 2.0;
        lat += (2.5 / 60.0) / 2.0;
        return std::make_pair(lat, lon);
    }

    // Extended subsquare pair (8-char) — 10×10 of 30" lon × 15" lat.
    lon += (C(6) - '0') * (30.0 / 3600.0);
    lat += (C(7) - '0') * (15.0 / 3600.0);
    lon += (30.0 / 3600.0) / 2.0;
    lat += (15.0 / 3600.0) / 2.0;
    return std::make_pair(lat, lon);
}

} // namespace lyra::ham
