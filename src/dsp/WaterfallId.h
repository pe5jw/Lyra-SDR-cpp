// Lyra — #175 TX Waterfall callsign ID: raster generator.
//
// Pure, self-contained text→audio generator for the waterfall Auto-ID.
// Given a callsign string it produces a MONO float buffer (at the requested
// sample rate) that, when transmitted as SSB audio, paints the text as a
// readable image in the upper part of the receiver's waterfall.
//
// No wiring / no TX here — this is the engine only (build increment 1).  The
// standalone keyed-burst orchestration (mute mic → feed this → flat TX →
// key/paint/unkey) is increment 2; see docs/architecture/wf_id_design.md.
//
// Glyphs come from Qt text rasterisation (QPainter→QImage, antialiasing OFF)
// rather than a hand-maintained bitmap font: correct for any character, with
// nothing to keep in sync.

#pragma once

#include <cstdint>
#include <vector>

#include <QString>

namespace lyra::dsp {

struct WaterfallIdParams {
    // Waterfall axes: image COLUMN x → frequency (the reading direction =
    // horizontal on the RX waterfall, spread across [bandLow, bandHigh]);
    // image ROW y → time (vertical scroll).  So the call reads upright,
    // left-to-right across the passband, painted top→bottom over `rows`
    // time-steps.
    double sampleRate = 48000.0;   // output rate (match the TX audio in-rate)
    double bandLowHz  = 500.0;     // low edge of the frequency span the call fills
    double bandHighHz = 2500.0;    // high edge (kept inside the TX passband)
    double stepMs     = 28.0;      // dwell per text ROW = per waterfall time-step
    double rampMs     = 1.5;       // per-tone gate slew — anti-click (tighter = crisper edges)
    double level      = 0.06;      // output level (BENCH-TUNED lean — keep power low; ALC limits)
    int    rows       = 48;        // rendered text HEIGHT in px = time resolution (≤64)
    // LSB inverts the audio→RF mapping vs USB, so the same low→high audio
    // layout comes out MIRRORED left↔right on the RX waterfall.  Set true for
    // LSB (the controller forces DIGL) to pre-reverse the frequency layout so
    // the modulation flip cancels it and the call reads upright on the air.
    bool   reverseFreq = false;
};

class WaterfallId {
public:
    // Render `text` (e.g. the operator's callsign) to a mono float buffer at
    // p.sampleRate.  Returns an empty buffer for empty/blank text or a band
    // that can't hold `rows` bins.  Output is clamped to [-1, 1] with a short
    // lead-in/out fade.
    static std::vector<float> render(const QString &text,
                                     const WaterfallIdParams &p);

    // Rasterise `text` to column bit-masks (bit r = pixel row r is on, row 0
    // = top), `rows` pixels tall, leading/trailing blank columns trimmed.
    // Exposed for testing.  rows is clamped to 1..64 (uint64 mask).
    static std::vector<std::uint64_t> rasterColumns(const QString &text,
                                                    int rows);
};

} // namespace lyra::dsp
