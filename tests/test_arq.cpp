#include "sdr/framing/ArqWindow.hpp"
#include <cassert>
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

void run_arq() {
    std::cout << "[arq tests]\n";
    test_send_ack_frees_slot();
    test_timeout_triggers_retransmit();
    test_max_retries_drops_frame();
    test_window_backpressure();
    std::cout << "[arq tests] ALL PASS\n\n";
}
