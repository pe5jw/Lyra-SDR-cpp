// Lyra — VOX gate (#91). See VoxDetector.h.

#include "VoxDetector.h"

#include <cmath>

namespace lyra::tx {

namespace {
// Linear RMS → dBFS.  Floor well below any real threshold so a silent
// input reads as "far under threshold" without a log(0).
inline double lin2dbfs(double x) noexcept {
    return x > 1e-9 ? 20.0 * std::log10(x) : -200.0;
}
}  // namespace

void VoxDetector::reset() noexcept {
    keyed_   = false;
    openAcc_ = 0.0;
    hangAcc_ = 0.0;
}

bool VoxDetector::tick(double micRmsLin, double rxRmsLin, double dtMs) noexcept {
    if (dtMs < 0.0) dtMs = 0.0;

    const double micDb    = lin2dbfs(micRmsLin);
    const double rxDb      = lin2dbfs(rxRmsLin);
    const bool   micAbove  = micDb >= p_.thresholdDbfs;
    const bool   antiBlock = p_.antiVoxOn && (rxDb >= p_.antiVoxDbfs);

    if (!keyed_) {
        // Closed: accumulate the open-delay only while the mic is above
        // threshold AND anti-VOX isn't suppressing.  Any lapse resets it
        // so a brief thump can't creep the accumulator toward open.
        if (micAbove && !antiBlock) {
            openAcc_ += dtMs;
            if (openAcc_ >= static_cast<double>(p_.openMs)) {
                keyed_   = true;
                hangAcc_ = static_cast<double>(p_.hangMs);
                openAcc_ = 0.0;
            }
        } else {
            openAcc_ = 0.0;
        }
    } else {
        // Open: mic-above refreshes the hold; quiet drains it.  Anti-VOX
        // is intentionally ignored here (RX is muted during TX).
        if (micAbove) {
            hangAcc_ = static_cast<double>(p_.hangMs);
        } else {
            hangAcc_ -= dtMs;
            if (hangAcc_ <= 0.0) {
                keyed_   = false;
                hangAcc_ = 0.0;
            }
        }
    }
    return keyed_;
}

}  // namespace lyra::tx
