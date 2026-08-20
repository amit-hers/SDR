#include "BridgeMode.hpp"
#include "sdr/framing/Frame.hpp"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <complex>

namespace sdr {

static constexpr int STAT_TICK = 100;   // ms

// "AUTO" (default) keeps SNR-based auto-switching; anything else locks the
// modem to that fixed scheme (e.g. to test a link whose frequency offset is
// too wide for high-order QAM to hold lock).
static std::optional<ModScheme> parseForcedModulation(const std::string& s) {
    if (s == "BPSK")  return ModScheme::BPSK;
    if (s == "QPSK")  return ModScheme::QPSK;
    if (s == "16QAM") return ModScheme::QAM16;
    if (s == "64QAM") return ModScheme::QAM64;
    return std::nullopt;   // "AUTO" or unrecognized
}

BridgeMode::BridgeMode(const Config& cfg, PlutoSDR& radio)
    : cfg_(cfg), radio_(radio)
{
    tap_ = TUNTAPDevice::create(cfg_.tap_iface, /*tap=*/true);
    tap_->setMTU(cfg_.tap_mtu);
    if (!cfg_.bridge_iface.empty())
        tap_->addToBridge(cfg_.bridge_iface, cfg_.lan_iface);

    amod_ = std::make_unique<AdaptiveModem>(parseForcedModulation(cfg_.modulation));
    if (cfg_.fec)     fec_ = std::make_unique<ReedSolomon>();
    if (cfg_.encrypt) aes_ = std::make_unique<AESCipher>(cfg_.aes_key_bytes.data());
    fft_ = std::make_unique<FFTSpectrum>();
    if (cfg_.arq) {
        ArqWindow::Config acfg;
        acfg.window_size = cfg_.arq_window;
        acfg.timeout_ms  = cfg_.arq_timeout_ms;
        acfg.max_retries = cfg_.arq_max_retries;
        arq_ = std::make_unique<ArqWindow>(acfg);
    }

    if (const char* path = std::getenv("SDR_FRAME_LOG"); path && *path)
        frame_log_.open(path, std::ios::trunc);
    if (const char* path = std::getenv("SDR_RAW_LOG"); path && *path)
        raw_log_.open(path, std::ios::trunc);
}

BridgeMode::~BridgeMode() { stop(); }

void BridgeMode::start() {
    if (running_.exchange(true)) return;
    stats_.uptime_s.store(0);
    tx_thread_      = std::thread(&BridgeMode::txThread,      this);
    capture_thread_ = std::thread(&BridgeMode::captureThread, this);
    rx_thread_      = std::thread(&BridgeMode::rxThread,      this);
    stat_thread_    = std::thread(&BridgeMode::statThread,    this);
}

void BridgeMode::stop() {
    running_.store(false);
    capture_cv_.notify_all();   // wake the DSP thread out of its wait
    if (tx_thread_.joinable())      tx_thread_.join();
    if (capture_thread_.joinable()) capture_thread_.join();
    if (rx_thread_.joinable())      rx_thread_.join();
    if (stat_thread_.joinable())    stat_thread_.join();
}

// ── TX thread ────────────────────────────────────────────────────────────────
void BridgeMode::txThread() {
    std::vector<uint8_t>             pkt(MAX_PAYLOAD);
    std::vector<std::complex<float>> iq_syms, iq_shaped;
    std::vector<int16_t>             iq_hw;
    RRCInterp interp;

    // A txPush costs the full tx-buffer length in airtime no matter how few
    // samples were written, so frames are staged here and flushed together.
    // Without this, a short frame wastes >98% of the air time on zero padding
    // and the link tops out around 15 frames/sec regardless of symbol rate.
    const size_t tx_capacity = radio_.txCapacity();
    std::vector<int16_t> stage;
    stage.reserve(tx_capacity * 2);

    auto flush = [&]() {
        if (stage.empty()) return;
        radio_.txPush(stage.data(), stage.size() / 2);
        stage.clear();
    };

    auto transmit = [&](const std::vector<uint8_t>& frame) {
        amod_->modulate(frame.data(), static_cast<int>(frame.size()), iq_syms);
        interp.process(iq_syms, iq_shaped);

        iq_hw.resize(iq_shaped.size() * 2);
        for (size_t i = 0; i < iq_shaped.size(); ++i) {
            iq_hw[i * 2]     = static_cast<int16_t>(iq_shaped[i].real() * 2047.f);
            iq_hw[i * 2 + 1] = static_cast<int16_t>(iq_shaped[i].imag() * 2047.f);
        }

        // Flush first if this frame wouldn't fit, then stage it. A frame
        // larger than the whole buffer is pushed on its own (txPush clamps).
        if (stage.size() / 2 + iq_shaped.size() > tx_capacity) flush();
        if (iq_shaped.size() > tx_capacity) {
            radio_.txPush(iq_hw.data(), iq_shaped.size());
            return;
        }
        stage.insert(stage.end(), iq_hw.begin(), iq_hw.end());
    };

    bool                  have_pending{false};
    uint32_t              pending_seq{0};
    size_t                pending_payload_len{0};
    std::vector<uint8_t>  pending_frame;

    while (running_.load()) {
        if (cfg_.arq) {
            // Drain queued ACK/control frames first — cheap, and keeps the
            // peer's retransmit timers from firing unnecessarily.
            while (auto* slot = ctrl_ring_.peek()) {
                transmit(std::vector<uint8_t>(slot->data, slot->data + slot->len));
                ctrl_ring_.consume();
            }
            // Retransmit anything whose ACK timeout elapsed.
            for (auto& pf : arq_->pollTimeouts())
                transmit(pf.encoded_bytes);

            stats_.arq_acked      .store(arq_->acked(),       std::memory_order_relaxed);
            stats_.arq_retransmits.store(arq_->retransmits(), std::memory_order_relaxed);
            stats_.arq_dropped    .store(arq_->dropped(),     std::memory_order_relaxed);
        }

        if (!have_pending) {
            ssize_t n = tap_->read(pkt.data(), pkt.size());
            if (n <= 0) {
                // TAP is non-blocking and currently empty: nothing more is
                // coming right now, so send whatever is staged rather than
                // holding it until the buffer happens to fill. Then idle
                // briefly -- spinning here would burn a core that the
                // capture and DSP threads need.
                flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            uint8_t flags = 0;
            if (cfg_.fec)     flags |= FL_FEC;
            if (cfg_.encrypt) flags |= FL_ENCRYPT;
            pending_seq = tx_seq_.fetch_add(1, std::memory_order_relaxed);
            ModCode mod = amod_->currentModCode();

            Framer framer;
            pending_frame = framer.encode(pkt.data(), static_cast<size_t>(n),
                                          flags, mod, mhzToBw(cfg_.bw_mhz),
                                          cfg_.node_id_u32, pending_seq,
                                          fec_.get(), aes_.get());
            pending_payload_len = static_cast<size_t>(n);
            have_pending = true;
        }

        if (cfg_.arq && !arq_->trySend(pending_seq, pending_frame))
            continue; // window full — hold the packet, retry next loop

        transmit(pending_frame);
        stats_.frames_tx.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_tx .fetch_add(static_cast<uint64_t>(pending_payload_len),
                                   std::memory_order_relaxed);
        have_pending = false;
    }
}

// ── Capture thread ───────────────────────────────────────────────────────────
// Nothing but rxPull -> queue. Keeping this free of DSP work is what stops
// the receiver going deaf mid-burst (see CAPTURE_QUEUE_MAX in the header).
void BridgeMode::captureThread() {
    while (running_.load()) {
        std::vector<int16_t> buf(IQ_SAMPLES * 2);

        auto t0 = std::chrono::steady_clock::now();
        int n = radio_.rxPull(buf.data(), IQ_SAMPLES);
        auto t1 = std::chrono::steady_clock::now();

        auto us = [](auto a, auto b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
        };
        stats_.rx_pull_us .fetch_add(us(t0, t1), std::memory_order_relaxed);
        stats_.rx_total_us.fetch_add(us(t0, std::chrono::steady_clock::now()),
                                     std::memory_order_relaxed);

        if (n <= 0) continue;
        buf.resize(static_cast<size_t>(n) * 2);

        {
            std::lock_guard<std::mutex> lk(capture_mu_);
            if (capture_queue_.size() >= CAPTURE_QUEUE_MAX) {
                capture_queue_.pop_front();          // DSP can't keep up
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            capture_queue_.push_back(std::move(buf));
        }
        capture_cv_.notify_one();
    }
    capture_cv_.notify_all();   // release a waiting DSP thread on shutdown
}

// ── RX thread ────────────────────────────────────────────────────────────────
void BridgeMode::rxThread() {
    std::vector<int16_t>             iq_hw;
    std::vector<std::complex<float>> iq_f, window_buf, offset_buf, iq_timed, iq_syms;
    AGC agc; TimingSync tsync; CostasLoop costas;
    BurstDetector::Config bcfg;
    bcfg.block_size  = static_cast<size_t>(cfg_.burst_block);
    bcfg.threshold_x = static_cast<float>(cfg_.burst_threshold);
    bcfg.margin      = static_cast<size_t>(cfg_.burst_margin);
    bcfg.merge_gap   = static_cast<size_t>(cfg_.burst_merge_gap);
    BurstDetector detector(bcfg);
    uint64_t logged_crc_errors = 0;
    const double sample_rate = static_cast<double>(cfg_.bw_mhz) * 4e6;

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(capture_mu_);
            capture_cv_.wait(lk, [&] {
                return !capture_queue_.empty() || !running_.load();
            });
            if (!running_.load() && capture_queue_.empty()) break;
            iq_hw = std::move(capture_queue_.front());
            capture_queue_.pop_front();
        }

        int n = static_cast<int>(iq_hw.size() / 2);
        if (n <= 0) continue;

        iq_f.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            iq_f[static_cast<size_t>(i)] = {
                iq_hw[static_cast<size_t>(i) * 2]     / 2048.f,
                iq_hw[static_cast<size_t>(i) * 2 + 1] / 2048.f };

        fft_->accumulate(iq_f.data(), n);
        if (fft_->ready()) {
            auto& spec = fft_->get();
            for (size_t i = 0; i < spec.size(); ++i)
                stats_.spectrum[i] = spec[i];
        }

        // A real transmission is a sparse, short burst inside a mostly-
        // silent batch. Feeding AGC/TimingSync/CostasLoop the whole batch
        // continuously lets their state adapt to whatever's dominant (the
        // silence), not the brief real signal. Detect and isolate the
        // actual burst window(s) first, and reset the chain fresh for each
        // one, instead of running it blindly across the whole batch.
        auto windows = detector.detect(iq_f);
        if (windows.empty()) continue; // nothing here -- skip the DSP chain entirely
        stats_.bursts_detected.fetch_add(windows.size(), std::memory_order_relaxed);

        for (const auto& win : windows) {
            window_buf.assign(iq_f.begin() + static_cast<long>(win.start),
                              iq_f.begin() + static_cast<long>(win.end));

            // Blind coarse CFO correction: two independent free-running
            // TCXOs (no shared reference clock) can produce an offset well
            // outside CostasLoop's native (and non-monotonic past that
            // range) pull-in range. Collapse it to a small residual before
            // AGC/Costas. Cheaper and more accurate now that it runs on an
            // isolated window instead of the whole batch.
            CoarseFreqCorrect::apply(window_buf, sample_rate);

            // TimingSync cannot reliably acquire symbol timing from an
            // arbitrary start offset within a burst: whether it locks is a
            // brittle function of where the window happens to begin
            // relative to the RRC tap grid (measured: some offsets decode
            // 100% of the time, immediate neighbours 0%). A detected burst
            // window starts at an essentially arbitrary offset, so retry
            // the demod at each sub-grid alignment and take the first one
            // that yields a frame. Sweeping all RRC_TAPS offsets recovers
            // every frame that the naive single-offset attempt misses.
            // Cost is bounded: this runs only on short, sparse burst
            // windows, not on the whole batch.
            bool decoded_any = false;
            for (size_t offset = 0; offset < DECODE_OFFSETS && !decoded_any; ++offset) {
                if (offset >= window_buf.size()) break;
                offset_buf.assign(window_buf.begin() + static_cast<long>(offset),
                                  window_buf.end());

                // Fresh cold start per attempt -- do not carry over state
                // adapted to the preceding silence or a failed alignment.
                agc.reset(); tsync.reset(); costas.reset();

                agc.process(offset_buf);                 // in-place
                // NOTE: TimingSync is itself an RRC matched-filter + decimator
                // (symsync_crcf expects oversampled input); do not RRCDecim first.
                tsync.process(offset_buf, iq_timed);      // matched filter + symbol timing
                costas.process(iq_timed, iq_syms);        // carrier recovery

                std::vector<uint8_t> bits;
                amod_->demodulate(iq_syms.data(), static_cast<int>(iq_syms.size()), bits);

                if (raw_log_.is_open() && !bits.empty()) {
                    static char hex[] = "0123456789abcdef";
                    std::string line;
                    line.reserve(bits.size() * 3);
                    for (uint8_t b : bits) {
                        line += hex[b >> 4]; line += hex[b & 0xF]; line += ' ';
                    }
                    raw_log_ << line << "\n";
                    raw_log_.flush();
                }

                float rssi = agc.rssi_dbm();
                float snr  = rssi + 95.f;
                stats_.rssi_dbm.store(rssi, std::memory_order_relaxed);
                stats_.snr_db  .store(snr,  std::memory_order_relaxed);
                amod_->updateSNR(snr);
                stats_.cur_mod .store(static_cast<int>(amod_->currentModCode()),
                                      std::memory_order_relaxed);

                // A misaligned attempt produces garbage that would poison a
                // shared Deframer's state machine -- use a fresh one per
                // alignment attempt.
                Deframer deframer;
                for (uint8_t byte : bits) {
                    auto result = deframer.push(byte, fec_.get(), aes_.get());

                    if (deframer.crcErrors() > logged_crc_errors) {
                        logged_crc_errors = deframer.crcErrors();
                        stats_.frames_rx_bad.fetch_add(1, std::memory_order_relaxed);
                        if (frame_log_.is_open())
                            frame_log_ << "[CRC-FAIL] total_so_far=" << logged_crc_errors
                                       << " rssi=" << rssi << " snr=" << snr << "\n";
                    }

                    if (!result) continue;
                    decoded_any = true;
                    stats_.updatePeer(result->node_id, rssi, snr);

                    if ((result->flags & FL_CTRL) && (result->flags & FL_ACK)) {
                        // Control frame: acknowledges our outgoing seq, no TAP payload.
                        if (cfg_.arq && arq_) arq_->onAck(result->seq);
                        continue;
                    }

                    if (frame_log_.is_open()) {
                        std::string content;
                        for (uint8_t b : result->payload)
                            content += (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                        frame_log_ << "[GOOD] seq=" << result->seq
                                   << " node=0x" << std::hex << result->node_id << std::dec
                                   << " len=" << result->payload.size()
                                   << " rssi=" << rssi << " snr=" << snr
                                   << " content=\"" << content << "\"\n";
                        frame_log_.flush();
                    }

                    ssize_t w = tap_->write(result->payload.data(), result->payload.size());
                    if (w > 0) {
                        stats_.frames_rx_good.fetch_add(1, std::memory_order_relaxed);
                        stats_.bytes_rx.fetch_add(static_cast<uint64_t>(w),
                                                  std::memory_order_relaxed);
                        if (result->flags & FL_FEC)
                            stats_.fec_corrected.fetch_add(1, std::memory_order_relaxed);
                    }

                    if (cfg_.arq) {
                        Framer ackFramer;
                        auto ack = ackFramer.encode(std::vector<uint8_t>{},
                                                    FL_CTRL | FL_ACK,
                                                    amod_->currentModCode(),
                                                    mhzToBw(cfg_.bw_mhz),
                                                    cfg_.node_id_u32, result->seq);
                        ctrl_ring_.push(ack.data(), static_cast<int>(ack.size()));
                    }
                }
            }
            if (decoded_any)
                stats_.bursts_demodulated.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ── Stat thread ───────────────────────────────────────────────────────────────
void BridgeMode::statThread() {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto last  = start;
    uint64_t prev_tx = 0, prev_rx = 0;
    int temp_tick = 0;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(STAT_TICK));
        auto now  = clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        uint64_t tx = stats_.bytes_tx.load(std::memory_order_relaxed);
        uint64_t rx = stats_.bytes_rx.load(std::memory_order_relaxed);
        stats_.tx_kbps.store(static_cast<float>((tx - prev_tx) * 8 / dt / 1e3),
                             std::memory_order_relaxed);
        stats_.rx_kbps.store(static_cast<float>((rx - prev_rx) * 8 / dt / 1e3),
                             std::memory_order_relaxed);
        prev_tx = tx; prev_rx = rx;

        stats_.uptime_s.store(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - start).count()),
            std::memory_order_relaxed);

        // Read chip temperature every 5 s (50 × 100 ms ticks)
        if (++temp_tick >= 50) {
            temp_tick = 0;
            stats_.temp_c.store(radio_.getTemp(), std::memory_order_relaxed);
        }
    }
}

} // namespace sdr
