// Lyra — Maidenhead grid-locator helpers (ported from old Lyra
// lyra/ham/grid.py).  Pure conversion math used to derive the operator's
// lat/lon from their grid square (consumed by the weather sources and the
// solar/propagation panel).  Native C++ — no Python.

#pragma once

#include <QString>

#include <optional>
#include <utility>

namespace lyra::ham {

// Valid Maidenhead: AA00, AA00aa, or AA00aa00 (field/square/[subsquare]/
// [extended]), case-insensitive.
bool isValidGrid(const QString &grid);

// Uppercased + trimmed grid if valid, else "" (old-Lyra normalize_grid).
QString normalizeGrid(const QString &grid);

// Latitude/longitude of the grid cell centre, or nullopt if invalid.
// Returns {lat, lon} (note the order — matches old Lyra grid_to_latlon).
std::optional<std::pair<double, double>> gridToLatLon(const QString &grid);

} // namespace lyra::ham
