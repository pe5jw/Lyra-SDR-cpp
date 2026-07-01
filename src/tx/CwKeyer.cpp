// Lyra — CW keyer element pump (#105 CW-3a). See CwKeyer.h.

#include "CwKeyer.h"

#include <chrono>

namespace lyra::tx {

using clock = std::chrono::steady_clock;

CwKeyer::CwKeyer(BitFn keyFn, BitFn pttFn, StateFn onState)
    : keyFn_(std::move(keyFn)),
      pttFn_(std::move(pttFn)),
      onStateFn_(std::move(onState)) {
    th_ = std::thread([this] { run(); });
}

CwKeyer::~CwKeyer() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_  = true;
        abort_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (th_.joinable())
        th_.join();
}

void CwKeyer::send(const std::string& text, int wpm, int weightPct) {
    auto elems = cwTextToElements(text, wpm, weightPct);
    if (elems.empty())
        return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (stop_)
            return;
        queue_.insert(queue_.end(), elems.begin(), elems.end());
    }
    cv_.notify_all();
}

void CwKeyer::abort() {
    {
        std::lock_guard<std::mutex> lk(m_);
        abort_ = true;
        queue_.clear();
    }
    cv_.notify_all();
}

void CwKeyer::run() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        // Idle: wait for work (or shutdown).
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_)
            return;

        // A fresh message begins. abort_ may be stale-true from a
        // prior aborted run; clear it for this message.
        abort_   = false;
        busy_.store(true, std::memory_order_relaxed);

        lk.unlock();
        if (onStateFn_) onStateFn_(true);
        pttFn_(true);                       // hold cwx_ptt for the message
        lk.lock();

        // Absolute-deadline element walk (jitter does not accumulate).
        auto deadline = clock::now();
        for (;;) {
            if (abort_ || stop_)
                break;
            if (queue_.empty())
                break;                      // message drained
            CwElement e = queue_.front();
            queue_.erase(queue_.begin());

            lk.unlock();
            keyFn_(e.key);                  // cwx = mark/space
            lk.lock();

            deadline += std::chrono::microseconds(e.durationUs);
            // Interruptible hold until this element's deadline.
            cv_.wait_until(lk, deadline, [this] { return abort_ || stop_; });
        }

        const bool aborted = abort_ || stop_;
        lk.unlock();
        keyFn_(false);                      // ensure key released
        pttFn_(false);                      // drop cwx_ptt (gateware hang covers tail)
        if (onStateFn_) onStateFn_(false);
        lk.lock();

        busy_.store(false, std::memory_order_relaxed);
        if (aborted) {
            queue_.clear();
            abort_ = false;
        }
        if (stop_)
            return;
    }
}

}  // namespace lyra::tx
