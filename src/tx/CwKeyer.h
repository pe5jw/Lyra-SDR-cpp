// Lyra — CW keyer element pump (#105 CW-3a, host software keyer).
//
// Owns a dedicated worker thread that walks a CwMorse element list on
// a monotonic absolute-deadline schedule (locked decision B,
// cw3_software_keyer_design.md §4) and drives two injected callbacks:
//   - keyFn(on):  the per-element CW key  → wire `tx[0].cwx`
//   - pttFn(on):  the message-level hold  → wire `tx[0].cwx_ptt`
// The gateware keys the carrier on `cwx` (cwx_keydown) and holds TX
// between elements via `cwx_ptt` + its 500-unit spacing hang
// (hl2_rtl_dsopenhpsdr1.v:422); the host need only toggle these bits.
//
// Callbacks are injected (std::function) so the keyer has no hard
// dependency on HL2Stream and is unit-testable with capture lambdas.
// The keyer thread calls them; the callee (HL2Stream::setCwx*) is
// responsible for its own prn-field synchronisation vs the EP2 writer.
//
// Absolute-deadline scheduling: each element holds its bit until a
// steady_clock deadline that accumulates element durations, so
// per-element scheduler jitter does not drift the overall WPM. The
// wait is condvar-interruptible so abort() / shutdown break a
// long dah immediately.
//
// Reference model: the CWX engine's element-rate timer-driven
// state machine (cwx.cs:257-272 timer, :1805-1808 / :2195-2276
// process_element; :313-323 quitshut abort). Lyra-native realisation.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CwMorse.h"

namespace lyra::tx {

class CwKeyer {
public:
    using BitFn   = std::function<void(bool)>;
    using StateFn = std::function<void(bool)>;

    // keyFn → cwx, pttFn → cwx_ptt. onState(true) fires just before
    // the first key of a message, onState(false) after the last
    // element + the cwx_ptt drop (the FSM/TX-state hook — break-in
    // policy lives in the callee). Spawns the worker thread.
    CwKeyer(BitFn keyFn, BitFn pttFn, StateFn onState = {});
    ~CwKeyer();

    CwKeyer(const CwKeyer&)            = delete;
    CwKeyer& operator=(const CwKeyer&) = delete;

    // Translate `text` at `wpm` / `weightPct` and enqueue its
    // elements. If a message is already playing, the new elements are
    // appended (continuous send, like a logger queuing). Thread-safe.
    void send(const std::string& text, int wpm, int weightPct = 50);

    // Immediately abort: flush the queue, drop key + cwx_ptt, end the
    // message. Safe to call from any thread (e.g. a paddle interrupt
    // or operator Stop). Thread-safe.
    void abort();

    bool busy() const noexcept { return busy_.load(std::memory_order_relaxed); }

private:
    void run();

    BitFn   keyFn_;
    BitFn   pttFn_;
    StateFn onStateFn_;

    std::mutex              m_;
    std::condition_variable cv_;
    std::vector<CwElement>  queue_;        // pending elements (guarded by m_)
    bool                    abort_ = false;
    bool                    stop_  = false;  // thread-exit request
    std::atomic<bool>       busy_{false};

    std::thread th_;
};

}  // namespace lyra::tx
