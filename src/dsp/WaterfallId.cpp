// Lyra — #175 TX Waterfall callsign ID: raster generator (increment 1).
// See WaterfallId.h.  Engine only — no TX wiring yet.

#include "dsp/WaterfallId.h"

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QImage>
#include <QPainter>

namespace lyra::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi    = 3.141592653589793238462643383279;   // MSVC: no M_PI
}

std::vector<std::uint64_t> WaterfallId::rasterColumns(const QString &text,
                                                      int rows)
{
    rows = std::clamp(rows, 1, 64);
    std::vector<std::uint64_t> cols;
    const QString s = text.trimmed();
    if (s.isEmpty())
        return cols;

    // HIGH-RESOLUTION raster (root-cause rebuild 2026-06-22): the TX-state
    // analyzer is a 32768-pt FFT (~5.9 Hz/bin at 192k) — it resolves FAR
    // finer than the old ~50×12-cell grid I built against the 4096-pt RX
    // analyzer.  Render at full size so the call is ~150 freq × up to 64
    // time cells = smooth, Lyra-grade lettering.  Clean bold sans (the
    // reference's #1, Arial Bold), Condensed for a narrow call, antialias
    // OFF for hard 1-bit edges, a small letter gap to keep glyphs separated.
    QFont font(QStringLiteral("Arial"));
    font.setBold(true);
    font.setStretch(QFont::Condensed);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 3.0);   // small gap between letters
    font.setStyleStrategy(QFont::NoAntialias);
    // Size the glyphs so the CAPITALS fill the row height (caps ≈ 0.7×em →
    // ~1.4× pixelSize); at rows=48 this is a ~67 px face = smooth curves.
    font.setPixelSize(std::max(6, rows * 7 / 5));

    const QFontMetrics fm(font);
    const int imgH = rows;
    const int imgW = std::max(8, fm.horizontalAdvance(s) + 4);   // sized to the text
    QImage img(imgW, imgH, QImage::Format_Grayscale8);
    img.fill(0);   // black background
    {
        QPainter pr(&img);
        pr.setRenderHint(QPainter::Antialiasing, false);
        pr.setRenderHint(QPainter::TextAntialiasing, false);
        pr.setFont(font);
        pr.setPen(Qt::white);
        // Vertically centre the capitals in the row height so they fill it.
        const int cap      = fm.capHeight() > 0 ? fm.capHeight() : (imgH * 7 / 10);
        const int baseline = (imgH + cap) / 2;
        pr.drawText(2, baseline, s);
    }

    // Scan columns left→right; a pixel is "on" above 50 % grey.
    auto colMask = [&](int x) -> std::uint64_t {
        std::uint64_t m = 0;
        for (int y = 0; y < imgH; ++y) {
            const uchar v = img.constScanLine(y)[x];
            if (v >= 128)
                m |= (1ull << y);   // bit y, row 0 = top
        }
        return m;
    };

    int first = -1, last = -1;
    std::vector<std::uint64_t> raw(static_cast<size_t>(imgW));
    for (int x = 0; x < imgW; ++x) {
        raw[static_cast<size_t>(x)] = colMask(x);
        if (raw[static_cast<size_t>(x)] != 0) {
            if (first < 0)
                first = x;
            last = x;
        }
    }
    if (first < 0)
        return cols;   // nothing drawn

    cols.reserve(static_cast<size_t>(last - first + 1));
    for (int x = first; x <= last; ++x)
        cols.push_back(raw[static_cast<size_t>(x)]);
    return cols;
}

std::vector<float> WaterfallId::render(const QString &text,
                                       const WaterfallIdParams &p)
{
    std::vector<float> out;
    const int timeSteps = std::clamp(p.rows, 1, 64);   // image HEIGHT = time
    if (p.sampleRate <= 0.0 || p.bandHighHz <= p.bandLowHz)
        return out;

    // Rasterise the call UPRIGHT, then reinterpret its axes for a waterfall:
    // image COLUMN x → frequency (reading direction, spread low→high across
    // the band); image ROW y → time.  rasterColumns(text, timeSteps) returns
    // one mask per image column (x = frequency bin), bit y = pixel(x,y) on.
    const std::vector<std::uint64_t> cols = rasterColumns(text, timeSteps);
    if (cols.empty())
        return out;
    const int W = static_cast<int>(cols.size());        // frequency bins (text width)

    const int spr = std::max(
        1, static_cast<int>(std::lround(p.sampleRate * p.stepMs / 1000.0)));  // samples / time-step
    const size_t total = static_cast<size_t>(timeSteps) * static_cast<size_t>(spr);
    out.assign(total, 0.0f);

    // Column x → frequency: text-left → LOW frequency (left of the band),
    // i.e. the straight (forward) sense.  Orientation pinned by elimination
    // on the N8SDR HL2+ waterfall 2026-06-22: of the four flip combos, only
    // {frequency FORWARD, time REVERSED} was untried after the other three
    // each read wrong — so this is it.
    std::vector<double> freq(static_cast<size_t>(W));
    for (int x = 0; x < W; ++x) {
        freq[static_cast<size_t>(x)] = (W == 1)
            ? 0.5 * (p.bandLowHz + p.bandHighHz)
            : p.bandLowHz + (p.bandHighHz - p.bandLowHz)
                                * (static_cast<double>(x) / (W - 1));
    }

    std::vector<double> phase(static_cast<size_t>(W), 0.0);
    std::vector<double> gain(static_cast<size_t>(W), 0.0);   // slewed 0..1 per bin
    const double slew = 1.0 /
        std::max(1.0, p.sampleRate * p.rampMs / 1000.0);

    for (size_t n = 0; n < total; ++n) {
        // Time axis: emit rasterised rows BOTTOM→TOP (yrow = last - ts) —
        // the time-reversed sense.  Paired with forward frequency this is
        // the by-elimination correct orientation (see the freq comment).
        const int ts   = static_cast<int>(n / static_cast<size_t>(spr));
        const int yrow = (timeSteps - 1) - ts;
        double acc    = 0.0;
        int    active = 0;
        for (int x = 0; x < W; ++x) {
            const double target =
                ((cols[static_cast<size_t>(x)] >> yrow) & 1ull) ? 1.0 : 0.0;
            double &g = gain[static_cast<size_t>(x)];
            g += std::clamp(target - g, -slew, slew);
            double &ph = phase[static_cast<size_t>(x)];
            ph += kTwoPi * freq[static_cast<size_t>(x)] / p.sampleRate;
            if (ph >= kTwoPi)
                ph -= kTwoPi;
            if (g > 0.0)
                acc += g * std::sin(ph);
            if (target > 0.5)
                ++active;
        }
        // A horizontal letter stroke lights many adjacent bins at once;
        // normalise by sqrt(active) so the crest factor stays bounded (no
        // hard clip → no ALC slam) while keeping per-bin brightness usable.
        const double norm = std::sqrt(static_cast<double>(std::max(1, active)));
        out[n] = static_cast<float>(std::clamp(p.level * acc / norm, -1.0, 1.0));
    }

    // Raised-cosine lead-in / lead-out so the whole burst keys cleanly.
    const int fade = std::min<int>(
        static_cast<int>(total / 2),
        std::max(1, static_cast<int>(std::lround(p.sampleRate * p.rampMs / 1000.0))));
    for (int i = 0; i < fade; ++i) {
        const double w = 0.5 * (1.0 - std::cos(kPi * i / fade));
        out[static_cast<size_t>(i)] *= static_cast<float>(w);
        out[total - 1 - static_cast<size_t>(i)] *= static_cast<float>(w);
    }
    return out;
}

} // namespace lyra::dsp
