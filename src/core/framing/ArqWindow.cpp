#include "sdr/framing/ArqWindow.hpp"
#include <algorithm>

namespace sdr {

bool ArqWindow::trySend(uint32_t seq, std::vector<uint8_t> encoded_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    if (static_cast<int>(outstanding_.size()) >= cfg_.window_size) return false;

    Entry e;
    e.timeout_ms = cfg_.timeout_ms;
    e.deadline   = Clock::now() + std::chrono::milliseconds(e.timeout_ms);
    e.encoded_bytes = std::move(encoded_bytes);
    outstanding_.emplace(seq, std::move(e));
    return true;
}

void ArqWindow::onAck(uint32_t seq) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = outstanding_.find(seq);
    if (it == outstanding_.end()) return;
    outstanding_.erase(it);
    acked_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<ArqWindow::PendingFrame> ArqWindow::pollTimeouts(Clock::time_point now) {
    std::vector<PendingFrame> due;
    std::lock_guard<std::mutex> lk(mu_);

    for (auto it = outstanding_.begin(); it != outstanding_.end(); ) {
        Entry& e = it->second;
        if (now < e.deadline) { ++it; continue; }

        if (e.retries >= cfg_.max_retries) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            it = outstanding_.erase(it);
            continue;
        }

        ++e.retries;
        e.timeout_ms = std::min(static_cast<int>(e.timeout_ms * 1.5), 500);
        e.deadline   = now + std::chrono::milliseconds(e.timeout_ms);
        retransmits_.fetch_add(1, std::memory_order_relaxed);
        due.push_back(PendingFrame{it->first, e.encoded_bytes});
        ++it;
    }
    return due;
}

bool ArqWindow::full() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(outstanding_.size()) >= cfg_.window_size;
}

int ArqWindow::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(outstanding_.size());
}

} // namespace sdr
