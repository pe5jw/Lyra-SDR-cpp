// Lyra — color palettes (256-entry RGB lookup tables).
//
// Ported from the Python-Lyra palettes (lyra/ui/palettes.py): each
// palette maps normalized signal strength 0..1 to an RGB colour, used
// to intensity-colour the panadapter trace (and later the waterfall).
// One addition over old Lyra: "Amber" (the warm deskHPSDR look).
//
// Index order == names() order == the Settings -> Visuals combo order.

#pragma once

#include <QStringList>

#include <array>

namespace lyra::palettes {

using Lut = std::array<std::array<unsigned char, 3>, 256>;

// Palette names in UI/display order (index-aligned with lut()).
const QStringList &names();

// 256-entry RGB LUT for the palette at `index` (clamped to range).
const Lut &lut(int index);

} // namespace lyra::palettes
