#include "BridgeMode.hpp"
#include "sdr/framing/Frame.hpp"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <complex>
#include <algorithm>
#include <iostream>

namespace sdr {

static constexpr int STAT_TICK = 100;   // ms

BridgeMode::BridgeMode(const Config& cfg, PlutoSDR& radio)
    : cfg_(cfg), radio_(radio)
{
    tap_ = TUNTAPDevice::create(cfg_.tap_iface, /*tap=*/true);
    tap_->setMTU(cfg_.tap_mtu);
    if (!cfg_.bridge_iface.empty())
        tap_->addToBridge(cfg_.bridge_iface, cfg_.lan_iface);

    tx_mod_ = cfg_.txModCode();
    std::cerr << "[sdr] TX payload modulation: " << modCodeName(tx_mod_)
              << " (" << modCodeBits(tx_mod_) << " bit/symbol); "
                 "acquisition is always BPSK. RX payload modulation is taken "
                 "from each frame header.\n";

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

    if (const char* m = std::getenv("SDR_RX_CARRIER"); m && *m) {
        std::string s(m);
        if      (s == "ls")        carrier_mode_ = CarrierMode::LS;
        else if (s == "costas")    carrier_mode_ = CarrierMode::COSTAS;
        else if (s == "ls+costas") carrier_mode_ = CarrierMode::LS_COSTAS;
        else if (s == "none")      carrier_mode_ = CarrierMode::NONE;
        else std::cerr << "[sdr] WARNING: SDR_RX_CARRIER='" << s
                       << "' is not one of ls|costas|ls+costas|none; "
                          "using ls+costas\n";
    }
    std::cerr << "[sdr] RX carrier recovery: "
              << (carrier_mode_ == CarrierMode::LS        ? "LS (data-aided, open loop)"
                : carrier_mode_ == CarrierMode::COSTAS    ? "Costas loop (cold start)"
                : carrier_mode_ == CarrierMode::LS_COSTAS ? "Costas loop seeded by LS estimate"
                                                          : "none")
              << "\n";

    if (const char* e = std::getenv("SDR_PROFILE"); e && *e && std::string(e) != "0") {
        prof_.enable(true);
        std::cerr << "[sdr] stage profiling ENABLED\n";
    }

    if (const char* path = std::getenv("SDR_FRAME_LOG"); path && *path)
        frame_log_.open(path, std::ios::trunc);
    if (const char* path = std::getenv("SDR_RAW_LOG"); path && *path)
        raw_log_.open(path, std::ios::trunc);
    if (const char* path = std::getenv("SDR_IQ_DUMP"); path && *path) {
        size_t mb = 128;
        if (const char* m = std::getenv("SDR_IQ_DUMP_MB"); m && *m)
            mb = static_cast<size_t>(std::strtoul(m, nullptr, 10));
        iq_dump_limit_ = mb * 1024 * 1024;
        iq_dump_.open(path, std::ios::binary | std::ios::trunc);
        std::cerr << "[sdr] IQ dump -> " << path << " (limit " << mb << " MB)\n";
    }
}

BridgeMode::~BridgeMode() { stop(); }

void BridgeMode::start() {
    if (running_.exchange(true)) return;
    stats_.uptime_s.store(0);
    prof_t0_   = std::chrono::steady_clock::now();
    prof_cpu0_ = StageProfiler::processCpuSeconds();
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

    if (prof_.enabled()) {
        double wall = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - prof_t0_).count();
        double cpu  = StageProfiler::processCpuSeconds() - prof_cpu0_;
        std::cerr << prof_.report(wall)
                  << "process CPU " << cpu << " s over " << wall << " s wall = "
                  << (wall > 0 ? 100.0 * cpu / wall : 0.0) << "% of one core"
                  << " | RSS " << StageProfiler::residentMb() << " MB\n"
                  << "frames tx=" << stats_.frames_tx.load()
                  << " rx_good=" << stats_.frames_rx_good.load()
                  << " rx_bad=" << stats_.frames_rx_bad.load()
                  << " bursts=" << stats_.bursts_detected.load()
                  << " | bytes tx=" << stats_.bytes_tx.load()
                  << " rx=" << stats_.bytes_rx.load() << "\n";
    }
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
        { StageProfiler::Scope sc(prof_, StageProfiler::TX_PUSH, stage.size() / 2);
          radio_.txPush(stage.data(), stage.size() / 2); }
        stage.clear();
    };

    auto transmit = [&](const std::vector<uint8_t>& frame) {
        // Acquisition section in BPSK, payload in tx_mod_. Never modulate the
        // preamble with the payload scheme: the receiver correlates against a
        // fixed BPSK reference and would not see the burst at all.
        { StageProfiler::Scope sc(prof_, StageProfiler::TX_MOD, frame.size() * 8);
          SplitModem::modulate(frame, tx_mod_, iq_syms); }
        { StageProfiler::Scope sc(prof_, StageProfiler::TX_RRC, iq_syms.size());
          interp.process(iq_syms, iq_shaped); }

        { StageProfiler::Scope sc(prof_, StageProfiler::TX_CONV, iq_shaped.size());
          iq_hw.resize(iq_shaped.size() * 2);
          for (size_t i = 0; i < iq_shaped.size(); ++i) {
              iq_hw[i * 2]     = static_cast<int16_t>(iq_shaped[i].real() * 2047.f);
              iq_hw[i * 2 + 1] = static_cast<int16_t>(iq_shaped[i].imag() * 2047.f);
          } }

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
            ssize_t n;
            { StageProfiler::Scope sc(prof_, StageProfiler::TX_TAPR);
              n = tap_->read(pkt.data(), pkt.size()); }
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

            Framer framer;
            StageProfiler::Scope sc_fr(prof_, StageProfiler::TX_FRAME, (uint64_t)n);
            pending_frame = framer.encode(pkt.data(), static_cast<size_t>(n),
                                          flags, tx_mod_, mhzToBw(cfg_.bw_mhz),
                                          cfg_.node_id_u32, pending_seq,
                                          fec_.get(), aes_.get());
            if (frame_log_.is_open())
                frame_log_ << "[TX] seq=" << pending_seq
                           << " payload=" << n
                           << " mod=" << modCodeName(tx_mod_)
                           << " wire=" << pending_frame.size() << "B\n";
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
        int n;
        { StageProfiler::Scope sc(prof_, StageProfiler::RX_PULL, IQ_SAMPLES);
          n = radio_.rxPull(buf.data(), IQ_SAMPLES); }
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

        // Diagnostic tap: write the raw capture stream to disk exactly as the
        // DSP thread will see it. The radio is held exclusively by this
        // process, so an external capture tool cannot observe a live link --
        // this is the only way to compare what the daemon actually receives
        // against what the offline analysis tools make of the same samples.
        // Written from the capture thread and bounded, so a long run cannot
        // fill the disk or stall reception.
        if (iq_dump_.is_open() && iq_dumped_ < iq_dump_limit_) {
            size_t want = std::min(buf.size() * sizeof(int16_t),
                                   iq_dump_limit_ - iq_dumped_);
            iq_dump_.write(reinterpret_cast<const char*>(buf.data()),
                           static_cast<std::streamsize>(want));
            iq_dumped_ += want;
            if (iq_dumped_ >= iq_dump_limit_) {
                iq_dump_.flush();
                std::cerr << "[sdr] IQ dump complete (" << iq_dumped_ / (1024 * 1024)
                          << " MB); further samples not recorded\n";
            }
        }

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
    bcfg.noise_quantile = static_cast<float>(cfg_.burst_noise_q);
    BurstDetector detector(bcfg);
    PreambleSync  psync(RRC_SPS);
    DataAidedSync dasync;
    uint64_t logged_crc_errors = 0;
    const double sample_rate = static_cast<double>(cfg_.bw_mhz) * 4e6;
    // The detector keeps burst_margin samples of context before the burst,
    // so the frame start is within roughly that plus a block. Bounding the
    // correlation search keeps its cost proportional to that, not to the
    // whole window.
    const size_t preamble_search =
        static_cast<size_t>(cfg_.burst_margin) + static_cast<size_t>(cfg_.burst_block) * 4;

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

        {
            StageProfiler::Scope sc(prof_, StageProfiler::RX_CONVERT, (uint64_t)n);
            iq_f.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
                iq_f[static_cast<size_t>(i)] = {
                    iq_hw[static_cast<size_t>(i) * 2]     / 2048.f,
                    iq_hw[static_cast<size_t>(i) * 2 + 1] / 2048.f };
        }

        // Spectrum is display-only. Rate-limit it: measured at 73% of all
        // process CPU when the link was idle, for something no decode path
        // reads. spectrum_interval_ms = 0 turns it off completely.
        if (cfg_.spectrum_interval_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_spectrum_ >=
                std::chrono::milliseconds(cfg_.spectrum_interval_ms)) {
                last_spectrum_ = now;
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_FFT, (uint64_t)n);
                  fft_->accumulate(iq_f.data(), n); }
                if (fft_->ready()) {
                    auto& spec = fft_->get();
                    for (size_t i = 0; i < spec.size(); ++i)
                        stats_.spectrum[i] = spec[i];
                }
            }
        }

        // A real transmission is a sparse, short burst inside a mostly-
        // silent batch. Feeding AGC/TimingSync/CostasLoop the whole batch
        // continuously lets their state adapt to whatever's dominant (the
        // silence), not the brief real signal. Detect and isolate the
        // actual burst window(s) first, and reset the chain fresh for each
        // one, instead of running it blindly across the whole batch.
        std::vector<BurstDetector::Window> windows;
        { StageProfiler::Scope sc(prof_, StageProfiler::RX_DETECT, iq_f.size());
          windows = detector.detect(iq_f); }
        if (windows.empty()) continue; // nothing here -- skip the DSP chain entirely
        stats_.bursts_detected.fetch_add(windows.size(), std::memory_order_relaxed);

        for (const auto& win : windows) {
            // The detector marks where energy is, which is not the same as
            // where the frame ends: pulse shaping and threshold hysteresis
            // routinely clip a window well short of a full frame, and
            // demodulating a fragment can never satisfy the CRC. Extend to
            // at least one maximum-size frame (plus slack for the alignment
            // search) so a located frame is always complete.
            const size_t max_frame_samples =
                (MAX_PAYLOAD + WIRE_FRAME_OVERHEAD) * 8 * static_cast<size_t>(RRC_SPS);
            size_t win_end = std::min(iq_f.size(),
                                      std::max(win.end, win.start + max_frame_samples
                                                        + static_cast<size_t>(cfg_.burst_margin)));
            window_buf.assign(iq_f.begin() + static_cast<long>(win.start),
                              iq_f.begin() + static_cast<long>(win_end));

            // Blind coarse CFO correction: two independent free-running
            // TCXOs (no shared reference clock) can produce an offset well
            // outside CostasLoop's native (and non-monotonic past that
            // range) pull-in range. Collapse it to a small residual before
            // AGC/Costas. Cheaper and more accurate now that it runs on an
            // isolated window instead of the whole batch.
            { StageProfiler::Scope sc(prof_, StageProfiler::RX_CFO, window_buf.size());
              CoarseFreqCorrect::apply(window_buf, sample_rate); }

            // TimingSync cannot reliably acquire symbol timing from an
            // arbitrary start offset: whether it locks is a brittle function
            // of where the window begins relative to the RRC tap grid
            // (measured: some offsets decode 100% of the time, immediate
            // neighbours 0%). Rather than retrying the whole DSP chain at
            // every alignment -- 32x the cost per burst, which saturates the
            // CPU and starves reception -- correlate against the known
            // preamble+sync-word waveform to *compute* the frame start, then
            // demodulate once there.
            //
            // A window with no real frame in it (noise, or a false burst
            // detection) fails the correlation cheaply instead of paying for
            // 32 full demod passes.
            // Under continuous traffic a single detected window spans many
            // back-to-back frames, so walk the window: locate a frame, decode
            // from there, then resume searching after it. Decoding only the
            // first frame per window would throw away most of a busy channel.
            bool decoded_any = false;
            size_t search_from = 0;
            for (int frame_no = 0; frame_no < MAX_FRAMES_PER_WINDOW; ++frame_no) {
                if (search_from >= window_buf.size()) break;

                std::vector<std::complex<float>> tail(
                    window_buf.begin() + static_cast<long>(search_from), window_buf.end());
                // Always bound the search. Letting it scan the whole tail
                // makes each correlation O(window * ref), and on a busy
                // channel the window is large and this loop runs many times
                // -- that alone starves the DSP thread. Frames follow each
                // other closely, so the next preamble is near at hand.
                PreambleSync::Match match;
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_PSYNC, tail.size());
                  match = psync.find(tail, preamble_search); }
                if (!match.found) break;

                const size_t base = search_from + static_cast<size_t>(match.offset);
                const size_t offsets[] = {
                    base,
                    // Neighbours, in case the peak sat a sample either side of
                    // the alignment TimingSync actually wants.
                    base > 0 ? base - 1 : 0,
                    base + 1,
                };

                bool this_frame_ok = false;
                for (size_t oi = 0; oi < 3 && !this_frame_ok; ++oi) {
                size_t offset = offsets[oi];
                if (offset >= window_buf.size()) break;
                offset_buf.assign(window_buf.begin() + static_cast<long>(offset),
                                  window_buf.end());

                // Fresh cold start per attempt -- do not carry over state
                // adapted to the preceding silence or a failed alignment.
                agc.reset(); tsync.reset(); costas.reset();

                { StageProfiler::Scope sc(prof_, StageProfiler::RX_AGC, offset_buf.size());
                  agc.process(offset_buf); }                 // in-place
                // NOTE: TimingSync is itself an RRC matched-filter + decimator
                // (symsync_crcf expects oversampled input); do not RRCDecim first.
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_TSYNC, offset_buf.size());
                  tsync.process(offset_buf, iq_timed); }     // matched filter + symbol timing

                // Carrier recovery. The default is a least-squares estimate
                // taken from the known BPSK acquisition symbols, then a single
                // open-loop derotation of the whole burst.
                //
                // A Costas loop has almost nothing to lock onto here: the
                // preamble is 0xAA, a perfectly alternating pattern, which in
                // BPSK is a suppressed carrier. Measured over a 472-frame
                // capture, correcting the measured phase ramp instead lifted
                // CRC-good from ~25% to ~65-77%, and cut mean corrupted
                // payload bytes from 64.5 to 4.9.
                //
                // $SDR_RX_CARRIER selects the method so the comparison stays
                // reproducible on hardware: ls (default) | costas | none.
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_CARRIER, iq_timed.size());
                switch (carrier_mode_) {
                    case CarrierMode::COSTAS:
                        costas.process(iq_timed, iq_syms);
                        break;
                    case CarrierMode::NONE:
                        iq_syms = iq_timed;
                        break;
                    case CarrierMode::LS: {
                        iq_syms = iq_timed;
                        auto est = dasync.estimate(iq_timed, DAS_SEARCH_SYMS);
                        // Deliberately open loop: no decision-directed
                        // tracking, so the gain from the estimate alone stays
                        // measurable in isolation.
                        if (est.ok) DataAidedSync::derotate(iq_syms, est);
                        break;
                    }
                    case CarrierMode::LS_COSTAS: {
                        // Seed the loop with the data-aided estimate instead
                        // of starting it cold. The LS fit supplies absolute
                        // phase and residual frequency measured on symbols the
                        // receiver already knows; the loop then only has to
                        // track what the open-loop extrapolation leaves behind,
                        // which is where the residual errors were concentrated
                        // (measured: 0% wrong in the first three eighths of the
                        // payload, 10.6% in the last).
                        auto est = dasync.estimate(iq_timed, DAS_SEARCH_SYMS);
                        if (est.ok) costas.seed(est.phase, est.freq_per_sym);
                        costas.process(iq_timed, iq_syms);
                        break;
                    }
                }
                }

                float rssi = agc.rssi_dbm();
                float snr  = rssi + 95.f;
                stats_.rssi_dbm.store(rssi, std::memory_order_relaxed);
                stats_.snr_db  .store(snr,  std::memory_order_relaxed);
                // NOTE: snr is reported only. It must never select a
                // modulation -- see tx_mod_ in the header for why.

                // Split demodulation: BPSK acquisition, then the payload in
                // whatever scheme the header declares. The receiver holds no
                // modulation state of its own.
                SplitModem::Result sm;
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_DEMOD, iq_syms.size());
                  sm = SplitModem::demodulate(iq_syms, fec_ != nullptr,
                                              SYNC_SEARCH_SYMS); }
                if (sm.header_ok) {
                    stats_.cur_mod.store(static_cast<int>(sm.payload_mod),
                                         std::memory_order_relaxed);
                    if (frame_log_.is_open())
                        frame_log_ << "[RX-HDR] mod=" << modCodeName(sm.payload_mod)
                                   << " plen=" << sm.plen
                                   << " flags=0x" << std::hex << int(sm.flags) << std::dec
                                   << " sync_sym=" << sm.sync_sym
                                   << (sm.inverted ? " inverted" : "")
                                   << (sm.complete ? " complete" : " TRUNCATED") << "\n";
                }
                if (!sm.complete) continue;   // nothing decodable at this offset

                if (raw_log_.is_open() && !sm.bytes.empty()) {
                    static char hex[] = "0123456789abcdef";
                    std::string line;
                    line.reserve(sm.bytes.size() * 3);
                    for (uint8_t b : sm.bytes) {
                        line += hex[b >> 4]; line += hex[b & 0xF]; line += ' ';
                    }
                    raw_log_ << line << "\n";
                    raw_log_.flush();
                }

                // A misaligned attempt produces garbage that would poison a
                // shared Deframer's state machine -- use a fresh one per
                // alignment attempt.
                Deframer deframer;
                StageProfiler::Scope sc_df(prof_, StageProfiler::RX_DEFRAME, sm.bytes.size());
                for (uint8_t byte : sm.bytes) {
                    auto result = deframer.push(byte, fec_.get(), aes_.get());

                    // The Deframer is recreated per alignment attempt, so its
                    // own error counter never exceeds 1. Comparing it against
                    // a running total (as this did) therefore recorded exactly
                    // one CRC failure for the entire process lifetime and made
                    // the failure rate look ~0 no matter how bad the link was.
                    // Count each fresh deframer's failure directly instead.
                    if (deframer.crcErrors() > 0) {
                        ++logged_crc_errors;
                        stats_.frames_rx_bad.fetch_add(1, std::memory_order_relaxed);
                        if (frame_log_.is_open())
                            frame_log_ << "[CRC-FAIL] total_so_far=" << logged_crc_errors
                                       << " plen=" << sm.plen
                                       << " mod=" << modCodeName(sm.payload_mod)
                                       << " rssi=" << rssi << " snr=" << snr << "\n";
                        break;   // this attempt is spent; try the next alignment
                    }

                    if (!result) continue;
                    decoded_any = true;
                    this_frame_ok = true;
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

                    ssize_t w;
                    { StageProfiler::Scope sc(prof_, StageProfiler::RX_TAPW, result->payload.size());
                      w = tap_->write(result->payload.data(), result->payload.size()); }
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
                                                    tx_mod_,
                                                    mhzToBw(cfg_.bw_mhz),
                                                    cfg_.node_id_u32, result->seq);
                        ctrl_ring_.push(ack.data(), static_cast<int>(ack.size()));
                    }
                }
                }   // end alignment-neighbour loop

                // Advance past this frame and look for the next one in the
                // same window. Even if the decode failed, step forward so a
                // stubborn correlation peak can't spin us in place.
                search_from = base + psync.referenceLength();
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
