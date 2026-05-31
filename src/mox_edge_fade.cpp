// Lyra-cpp — TX-1 component 5a: MoxEdgeFade implementation.
// See mox_edge_fade.h for the full design + safety rationale.

#include "mox_edge_fade.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lyra::dsp {

namespace {

// Cos² fade-in coefficient at phase k of N samples:
//   coef = 0.5 * (1 - cos(π * k / N))
// Rises smoothly 0 → 1.  coef(0) = 0, coef(N) = 1.  Hot path.
inline float cosineFadeIn(int phase, int N) noexcept {
    constexpr float kPi = static_cast<float>(std::numbers::pi);
    return 0.5f * (1.0f - std::cos(kPi * static_cast<float>(phase) /
                                          static_cast<float>(N)));
}

// Cos² fade-out coefficient at phase k of N samples:
//   coef = 0.5 * (1 + cos(π * k / N))
// Falls smoothly 1 → 0.  coef(0) = 1, coef(N) = 0.  Hot path.
inline float cosineFadeOut(int phase, int N) noexcept {
    constexpr float kPi = static_cast<float>(std::numbers::pi);
    return 0.5f * (1.0f + std::cos(kPi * static_cast<float>(phase) /
                                          static_cast<float>(N)));
}

} // namespace

int MoxEdgeFade::clampMs(int ms) noexcept {
    return std::clamp(ms, kMinFadeMs, kMaxFadeMs);
}

MoxEdgeFade::MoxEdgeFade() = default;

void MoxEdgeFade::setFadeInMs(int ms) noexcept {
    fadeInSamples_.store(msToSamples(clampMs(ms)),
                         std::memory_order_relaxed);
}

void MoxEdgeFade::setFadeOutMs(int ms) noexcept {
    fadeOutSamples_.store(msToSamples(clampMs(ms)),
                          std::memory_order_relaxed);
}

int MoxEdgeFade::fadeInMs() const noexcept {
    const int samples = fadeInSamples_.load(std::memory_order_relaxed);
    return (samples * 1000) / kSampleRateHz;
}

int MoxEdgeFade::fadeOutMs() const noexcept {
    const int samples = fadeOutSamples_.load(std::memory_order_relaxed);
    return (samples * 1000) / kSampleRateHz;
}

bool MoxEdgeFade::isIdle() const noexcept {
    return state_.load(std::memory_order_relaxed) == State::Idle;
}

bool MoxEdgeFade::isOn() const noexcept {
    return state_.load(std::memory_order_relaxed) == State::On;
}

void MoxEdgeFade::notifyMoxState(bool moxOn) noexcept {
    if (moxOn == prevMox_) {
        return;  // no edge — common case, fast path
    }
    prevMox_ = moxOn;

    if (moxOn) {
        // ---- Rising edge → FadingIn -----------------------------
        const State prev = state_.load(std::memory_order_relaxed);
        if (prev == State::FadingOut) {
            // Mid-fade reversal: preserve coef via cos² complement.
            // coef_out(phase_old, N_out) == coef_in(phase_new, N_in)
            //   → phase_new = N_in * (N_out - phase_old) / N_out
            const int Nin  = fadeInSamples_.load(std::memory_order_relaxed);
            const int Nout = fadeOutSamples_.load(std::memory_order_relaxed);
            if (Nout > 0) {
                const long long newPhase =
                    (static_cast<long long>(Nin) *
                     static_cast<long long>(Nout - phase_)) / Nout;
                phase_ = static_cast<int>(
                    std::clamp(newPhase,
                               static_cast<long long>(0),
                               static_cast<long long>(Nin)));
            } else {
                phase_ = 0;
            }
        } else {
            // Idle or On (impossible at edge-on-rise from On, but
            // handle defensively) → start fade from phase 0.
            phase_ = 0;
        }
        state_.store(State::FadingIn, std::memory_order_relaxed);
    } else {
        // ---- Falling edge → FadingOut ---------------------------
        const State prev = state_.load(std::memory_order_relaxed);
        if (prev == State::FadingIn) {
            // Mid-fade reversal: same cos² complement, in↔out swapped.
            const int Nin  = fadeInSamples_.load(std::memory_order_relaxed);
            const int Nout = fadeOutSamples_.load(std::memory_order_relaxed);
            if (Nin > 0) {
                const long long newPhase =
                    (static_cast<long long>(Nout) *
                     static_cast<long long>(Nin - phase_)) / Nin;
                phase_ = static_cast<int>(
                    std::clamp(newPhase,
                               static_cast<long long>(0),
                               static_cast<long long>(Nout)));
            } else {
                phase_ = 0;
            }
        } else {
            // On or Idle → start fade from phase 0.
            phase_ = 0;
        }
        state_.store(State::FadingOut, std::memory_order_relaxed);
    }
}

float MoxEdgeFade::advance() noexcept {
    switch (state_.load(std::memory_order_relaxed)) {
        case State::Idle:
            return 0.0f;

        case State::On:
            return 1.0f;

        case State::FadingIn: {
            const int N = fadeInSamples_.load(std::memory_order_relaxed);
            if (phase_ >= N) {
                // Fade complete — snap to On for subsequent calls.
                state_.store(State::On, std::memory_order_relaxed);
                phase_ = 0;
                return 1.0f;
            }
            const float coef = cosineFadeIn(phase_, N);
            ++phase_;
            return coef;
        }

        case State::FadingOut: {
            const int N = fadeOutSamples_.load(std::memory_order_relaxed);
            if (phase_ >= N) {
                // Fade complete — snap to Idle for subsequent calls.
                state_.store(State::Idle, std::memory_order_relaxed);
                phase_ = 0;
                return 0.0f;
            }
            const float coef = cosineFadeOut(phase_, N);
            ++phase_;
            return coef;
        }
    }
    return 0.0f;  // unreachable; defensive
}

} // namespace lyra::dsp
