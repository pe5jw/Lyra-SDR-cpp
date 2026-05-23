// Lyra — auto-fit dB range helper for the panadapter / waterfall.
//
// Models old Lyra's auto-scale (radio.py): the floor sits a fixed
// margin BELOW the noise floor (≈ 20th-percentile of the dB spectrum),
// the ceiling a margin ABOVE a slowly-decaying peak hold, and the span
// never collapses below a minimum.  The applied range is EWMA-smoothed
// so it tracks band activity without jittering.
//
// Header-only + Qt-free so both the Waterfall and the Panadapter can
// own one and compute independently from the dB spectrum they already
// read each frame (no cross-object QML binding to a live engine value —
// avoids the context-property binding-reactivity pitfalls seen
// elsewhere in this build).
//
// Feed the latest dB spectrum at a steady cadence (≈5 Hz is plenty);
// floorDb()/ceilDb() return the current applied range.

#pragma once

#include <algorithm>
#include <vector>

namespace lyra::ui {

class AutoScaler {
public:
    void feed(const float *spec, int n) {
        if (!spec || n < 4) {
            return;
        }
        // Noise floor — ~20th percentile of the dB values (nth_element
        // is O(n), no full sort).
        scratch_.assign(spec, spec + n);
        const int k = std::clamp(static_cast<int>(n * 0.20), 1, n - 1);
        std::nth_element(scratch_.begin(), scratch_.begin() + k,
                         scratch_.end());
        const double nf = scratch_[static_cast<size_t>(k)];

        // Peak with a slow decay: jump up instantly to a new peak, then
        // fall gradually so a brief transient keeps the ceiling raised
        // for a few seconds (old-Lyra rolling-peak window, done as a
        // decay so there's no history buffer to size).
        double pk = spec[0];
        for (int i = 1; i < n; ++i) {
            pk = std::max(pk, static_cast<double>(spec[i]));
        }
        if (pk >= peakHold_) {
            peakHold_ = pk;
        } else {
            peakHold_ = std::max(pk, peakHold_ - kPeakDecayDb);
        }

        double tFloor = nf - kNoiseHeadroomDb;
        double tCeil  = peakHold_ + kPeakHeadroomDb;
        if (tCeil - tFloor < kMinSpanDb) {
            tCeil = tFloor + kMinSpanDb;
        }

        if (!seeded_) {
            floor_ = tFloor;
            ceil_  = tCeil;
            seeded_ = true;
        } else {
            floor_ += kAlpha * (tFloor - floor_);
            ceil_  += kAlpha * (tCeil  - ceil_);
        }
    }

    double floorDb() const { return floor_; }
    double ceilDb()  const { return ceil_; }
    void   reset()   { seeded_ = false; peakHold_ = -200.0; }

private:
    // Margins + span match old Lyra (15 dB below NF, 15 dB above peak,
    // ≥50 dB span).  Decay/alpha are tuned for a ~5 Hz feed: ~1.25 dB/s
    // peak fall and a ~2 s smoothing time constant.
    static constexpr double kNoiseHeadroomDb = 15.0;
    static constexpr double kPeakHeadroomDb  = 15.0;
    static constexpr double kMinSpanDb       = 50.0;
    static constexpr double kPeakDecayDb     = 0.25;   // per feed (~5 Hz)
    static constexpr double kAlpha           = 0.10;   // EWMA per feed

    std::vector<float> scratch_;
    double floor_    = -130.0;
    double ceil_     =  -20.0;
    double peakHold_ = -200.0;
    bool   seeded_   = false;
};

} // namespace lyra::ui
