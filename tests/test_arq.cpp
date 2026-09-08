#include "sdr/framing/ArqWindow.hpp"
#include <cassert>
#include <random>
#include <map>
#include <iostream>
#include <thread>
#include <chrono>

using Clock = sdr::ArqWindow::Clock;

static void test_send_ack_frees_slot() {
    sdr::ArqWindow::Config cfg;
    cfg.window_size = 4;
    sdr::ArqWindow arq(cfg);

    assert(arq.trySend(0, {1, 2, 3}));
    assert(arq.size() == 1);
    arq.onAck(0);
    assert(arq.size() == 0);
    assert(arq.acked() == 1);
    std::cout << "  [arq] send/ack frees slot: PASS\n";
}

static void test_timeout_triggers_retransmit() {
    sdr::ArqWindow::Config cfg;
    cfg.window_size = 4;
    cfg.timeout_ms  = 10;
    cfg.max_retries = 5;
    sdr::ArqWindow arq(cfg);

    auto t0 = Clock::now();
    assert(arq.trySend(7, {9, 9, 9}));

    // Before the deadline: nothing due.
    assert(arq.pollTimeouts(t0).empty());

    // Past the deadline: frame reappears for retransmission.
    auto due = arq.pollTimeouts(t0 + std::chrono::milliseconds(11));
    assert(due.size() == 1);
    assert(due[0].seq == 7);
    assert((due[0].encoded_bytes == std::vector<uint8_t>{9, 9, 9}));
    assert(arq.retransmits() == 1);
    std::cout << "  [arq] timeout triggers retransmit: PASS\n";
}

static void test_max_retries_drops_frame() {
    sdr::ArqWindow::Config cfg;
    cfg.window_size = 4;
    cfg.timeout_ms  = 1;
    cfg.max_retries = 2;
    sdr::ArqWindow arq(cfg);

    auto t0 = Clock::now();
    assert(arq.trySend(1, {0xAB}));

    // Backoff is capped at 500ms, so +1s is always past the next deadline
    // regardless of how many times it has already backed off.
    auto poll = [&](int retry_n) {
        return arq.pollTimeouts(t0 + std::chrono::milliseconds(1000LL * (retry_n + 1)));
    };

    assert(poll(0).size() == 1);   // 1st retry
    assert(poll(1).size() == 1);   // 2nd retry (== max_retries)
    assert(poll(2).empty());       // 3rd timeout: dropped, not retransmitted
    assert(arq.size() == 0);
    assert(arq.dropped() == 1);
    std::cout << "  [arq] exceeding max_retries drops frame: PASS\n";
}

static void test_window_backpressure() {
    sdr::ArqWindow::Config cfg;
    cfg.window_size = 2;
    sdr::ArqWindow arq(cfg);

    assert(arq.trySend(0, {0}));
    assert(arq.trySend(1, {1}));
    assert(!arq.trySend(2, {2}));   // window full
    assert(arq.full());

    arq.onAck(0);
    assert(!arq.full());
    assert(arq.trySend(2, {2}));    // space freed by the ACK
    std::cout << "  [arq] window-full backpressure: PASS\n";
}

// Randomised property check. The scenario tests above each exercise one path;
// this drives the window with an arbitrary interleaving of sends, acks and
// timeouts and asserts the invariants that must hold at EVERY step, which is
// where accounting bugs in a retransmit window actually live.
static void test_invariants_under_random_traffic() {
    std::mt19937 rng(9876);
    for (int trial = 0; trial < 200; ++trial) {
        sdr::ArqWindow::Config cfg;
        cfg.window_size = 1 + int(rng() % 8);
        cfg.timeout_ms  = 1 + int(rng() % 20);
        cfg.max_retries = 1 + int(rng() % 4);
        sdr::ArqWindow w(cfg);

        auto now = sdr::ArqWindow::Clock::now();
        uint32_t next_seq = 0;
        size_t accepted = 0;
        std::map<uint32_t,int> seen;      // seq -> times retransmitted

        for (int step = 0; step < 300; ++step) {
            switch (rng() % 3) {
            case 0: {
                const bool was_full = w.full();
                const bool ok = w.trySend(next_seq, std::vector<uint8_t>(4, 0xAB));
                // trySend must succeed exactly when there was room.
                assert(ok == !was_full);
                if (ok) { ++accepted; ++next_seq; }
                break;
            }
            case 1:
                if (next_seq) w.onAck(rng() % next_seq);
                break;
            default: {
                now += std::chrono::milliseconds(1 + int(rng() % 40));
                for (auto& f : w.pollTimeouts(now)) {
                    int& n = seen[f.seq];
                    ++n;
                    // A frame may never be handed back more times than the
                    // retry budget allows.
                    assert(n <= cfg.max_retries);
                    assert(!f.encoded_bytes.empty());
                }
                break;
            }
            }
            // The window can never exceed its size, and size() must agree with
            // full().
            assert(w.size() >= 0);
            assert(w.size() <= cfg.window_size);
            assert(w.full() == (w.size() >= cfg.window_size));
        }

        // Conservation: everything accepted is acked, dropped, or still held.
        const uint64_t out = w.acked() + w.dropped() + uint64_t(w.size());
        assert(out == accepted);
    }
    std::cout << "  [arq] invariants hold under random traffic: PASS\n";
}

void run_arq() {
    std::cout << "[arq tests]\n";
    test_send_ack_frees_slot();
    test_timeout_triggers_retransmit();
    test_max_retries_drops_frame();
    test_window_backpressure();
    test_invariants_under_random_traffic();
    std::cout << "[arq tests] ALL PASS\n\n";
}
