#include "BridgeMode.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/dsp/FixedTimingSync.hpp"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <complex>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <pthread.h>
#include <sched.h>

namespace sdr {

// Write one [u32 seq][u32 len][bytes] record. Used to capture the exact
// CRC-covered body on TX and the exact pre-CRC byte stream on RX, so failed
// frames can be byte-diffed against what was actually sent.
static void dumpRecord(std::ofstream& f, uint32_t seq, const uint8_t* d, size_t n) {
    if (!f.is_open()) return;
    uint32_t len = static_cast<uint32_t>(n);
    f.write(reinterpret_cast<const char*>(&seq), 4);
    f.write(reinterpret_cast<const char*>(&len), 4);
    f.write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(n));
}

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

    {
        SoftwarePhy::Params pp;
        pp.bw_mhz             = cfg_.bw_mhz;
        pp.samples_per_symbol = cfg_.samples_per_symbol;
        pp.tx_duty_max        = cfg_.tx_duty_max;
        pp.node_id            = cfg_.node_id_u32;
        pp.tx_mod             = cfg_.txModCode();
        pp.fec                = cfg_.fec;
        pp.encrypt            = cfg_.encrypt;
        pp.aes_key            = cfg_.encrypt ? cfg_.aes_key_bytes.data() : nullptr;
        pp.rt_priority        = cfg_.rt_priority;
        pp.pin_cores          = cfg_.pin_cores;
        pp.rx_buffer_samples  = cfg_.rx_buffer_samples;
        pp.rx_queue_depth     = cfg_.rx_queue_depth;
        pp.burst_block        = cfg_.burst_block;
        pp.burst_threshold    = cfg_.burst_threshold;
        pp.burst_margin       = cfg_.burst_margin;
        pp.burst_merge_gap    = cfg_.burst_merge_gap;
        pp.burst_noise_q      = cfg_.burst_noise_q;
        pp.carrier_sense      = cfg_.carrier_sense;
        pp.carrier_sense_hold_ms = cfg_.carrier_sense_hold_ms;
        pp.spectrum_interval_ms  = cfg_.spectrum_interval_ms;
        pp.arq                = cfg_.arq;
        phy_ = std::make_unique<SoftwarePhy>(pp, radio_, prof_, stats_);

        // What happens to a recovered frame. This is the whole of the
        // daemon's receive path now: ARQ, TAP delivery, and the accounting
        // that goes with them. It sees frames, never samples -- which is what
        // makes it identical against a DTC radio.
        phy_->onFrame([this](const DecodedFrame& f) {
            if ((f.flags & FL_CTRL) && (f.flags & FL_ACK)) {
                // Acknowledges one of our outgoing seqs; carries no payload.
                if (cfg_.arq && arq_) arq_->onAck(f.seq);
                return;
            }
            ssize_t w = 0;
            if (f.flags & FL_AGGR) {
                // A short or failed TAP write is silent otherwise -- the
                // frame still counts as good because the first record landed.
                size_t ok = 0, failed = 0;
                size_t recs = aggregate::split(
                    f.payload.data(), f.payload.size(),
                    [&](const uint8_t* d, size_t l) {
                        ssize_t one = tap_->write(d, l);
                        if (one > 0) { w += one; ++ok; }
                        else         { ++failed; }
                    });
                rx_records_    .fetch_add(recs,   std::memory_order_relaxed);
                rx_rec_written_.fetch_add(ok,     std::memory_order_relaxed);
                rx_rec_failed_ .fetch_add(failed, std::memory_order_relaxed);
            } else {
                w = tap_->write(f.payload.data(), f.payload.size());
            }
            if (w > 0) {
                stats_.frames_rx_good.fetch_add(1, std::memory_order_relaxed);
                stats_.bytes_rx.fetch_add(static_cast<uint64_t>(w),
                                          std::memory_order_relaxed);
            }
            if (cfg_.arq) {
                // Hand the transmit side the seq to acknowledge; the PHY
                // frames it.
                const uint32_t ack_seq = f.seq;
                ctrl_ring_.push(reinterpret_cast<const uint8_t*>(&ack_seq),
                                static_cast<int>(sizeof(ack_seq)));
            }
        });
    }
    if (cfg_.fec)     fec_ = std::make_unique<ReedSolomon>();
    if (cfg_.encrypt) aes_ = std::make_unique<AESCipher>(cfg_.aes_key_bytes.data());
    if (cfg_.arq) {
        ArqWindow::Config acfg;
        acfg.window_size = cfg_.arq_window;
        acfg.timeout_ms  = cfg_.arq_timeout_ms;
        acfg.max_retries = cfg_.arq_max_retries;
        arq_ = std::make_unique<ArqWindow>(acfg);
    }

    if (cfg_.tx_duty_max > 0.0 && cfg_.tx_duty_max < 1.0)
        std::cerr << "[sdr] TX duty limit: " << (cfg_.tx_duty_max * 100.0)
                  << "% (gaps inserted between bursts so the peer can acquire)\n";
    else
        std::cerr << "[sdr] TX duty limit: DISABLED -- the channel may saturate\n";
    if (const char* e = std::getenv("SDR_PROFILE"); e && *e && std::string(e) != "0") {
        prof_.enable(true);
        std::cerr << "[sdr] stage profiling ENABLED\n";
    }
}

BridgeMode::~BridgeMode() { stop(); }

void BridgeMode::start() {
    if (running_.exchange(true)) return;
    stats_.uptime_s.store(0);
    prof_t0_   = std::chrono::steady_clock::now();
    prof_cpu0_ = StageProfiler::processCpuSeconds();
    tap_reader_thread_ = std::thread(&BridgeMode::tapReaderThread, this);
    phy_->start();
    tx_thread_      = std::thread(&BridgeMode::txThread,      this);
    stat_thread_    = std::thread(&BridgeMode::statThread,    this);
}

void BridgeMode::stop() {
    running_.store(false);
    // Wake EVERY waiter before joining anything.
    //
    // txThread can be blocked in flush() waiting for room on the sample
    // queue. Its predicate checks running_, but a predicate is only re-tested
    // on notify -- so notifying after join()ing that same thread deadlocks
    // the shutdown. Notify first, join second, always.
    tap_cv_.notify_all();       // wake the TX worker off the TAP queue
    tx_sq_cv_.notify_all();     // wake modulator and pusher off the sample queue
    if (tx_thread_.joinable())      tx_thread_.join();
    if (tap_reader_thread_.joinable()) tap_reader_thread_.join();
    if (phy_) phy_->stop();   // drains and accounts for its own staged samples
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
        // The daemon reports only what it owns: TAP ingress, aggregation and
        // ARQ. Everything below the boundary is the PHY's to account for.
        std::cerr << "TAP-PIPE tap_pkts=" << tx_tap_pkts_.load()
                  << " tap_bytes=" << tx_tap_bytes_.load()
                  << " tap_empty_polls=" << tx_tap_empty_.load()
                  << " -> sealed=" << tx_frames_sealed_.load()
                  << " arq_blocked=" << tx_arq_blocked_.load()
                  << " | tap_q_drops=" << tap_q_drops_.load()
                  << " hiwater=" << tap_q_hiwater_.load() << "\n";
        std::cerr << "TAP-WRITE records=" << rx_records_.load()
                  << " written=" << rx_rec_written_.load()
                  << " failed=" << rx_rec_failed_.load() << "\n";
        std::cerr << phy_->txReport(wall);
        std::cerr << phy_->rxReport(wall);
    }
}

void BridgeMode::applyRealtime(const char* who, int core) {
    if (cfg_.rt_priority > 0) {
        sched_param sp{};
        sp.sched_priority = cfg_.rt_priority;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0)
            std::cerr << "[sdr] " << who << ": SCHED_FIFO(" << cfg_.rt_priority
                      << ") failed (" << rc << ") -- needs root/CAP_SYS_NICE; "
                         "running at normal priority\n";
        else
            std::cerr << "[sdr] " << who << ": SCHED_FIFO priority "
                      << cfg_.rt_priority << "\n";
    }
    if (cfg_.pin_cores && core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(core), &set);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        std::cerr << "[sdr] " << who << ": pinned to core " << core
                  << (rc == 0 ? "" : " (FAILED)") << "\n";
    }
}

// ── TX thread ────────────────────────────────────────────────────────────────
// Drains modulated sample buffers to the radio, with duty pacing and carrier
// sense applied here rather than in the modulator. Runs in parallel with
// txThread so a blocking push never stalls modulation.
void BridgeMode::tapReaderThread() {
    applyRealtime("tapread", 2);
    std::vector<uint8_t> pkt(MAX_PAYLOAD);
    while (running_.load()) {
        ssize_t n = tap_->read(pkt.data(), pkt.size());
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(tap_mu_);
            if (tap_queue_.size() >= TAP_QUEUE_MAX) {
                // Drop the OLDEST. On a live datalink stale packets are worth
                // less than fresh ones, and dropping here is visible in
                // tap_q_drops_ rather than silently in the kernel queue.
                tap_queue_.pop_front();
                tap_q_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            tap_queue_.emplace_back(pkt.begin(), pkt.begin() + n);
            uint64_t sz = tap_queue_.size();
            if (sz > tap_q_hiwater_.load(std::memory_order_relaxed))
                tap_q_hiwater_.store(sz, std::memory_order_relaxed);
        }
        tap_cv_.notify_one();
    }
}

void BridgeMode::txThread() {
    std::vector<uint8_t> pkt(MAX_PAYLOAD);

    bool                  have_pending{false};
    uint32_t              pending_seq{0};
    size_t                pending_payload_len{0};
    uint8_t               pending_flags{0};
    std::vector<uint8_t>  pending_payload;

    // Aggregation buffer: several TAP packets, each length-prefixed, packed
    // into one RF payload. aggr_payload_bytes counts user bytes only, so the
    // throughput statistic stays comparable with the unaggregated case.
    std::vector<uint8_t>  aggr;
    size_t                aggr_count{0};
    size_t                aggr_payload_bytes{0};
    std::chrono::steady_clock::time_point aggr_started{};
    aggr.reserve(AGGR_TARGET_BYTES + MAX_PAYLOAD);

    // Builds one RF frame out of everything currently aggregated.
    auto sealAggregate = [&]() {
        if (aggr.empty()) return;
        // Flags are constant for data frames, which is what lets a retransmit
        // rebuild the identical frame from the payload alone.
        pending_flags = FL_AGGR;
        if (cfg_.fec)     pending_flags |= FL_FEC;
        if (cfg_.encrypt) pending_flags |= FL_ENCRYPT;
        pending_seq = tx_seq_.fetch_add(1, std::memory_order_relaxed);

        // No Framer here any more. The daemon hands the PHY a payload and the
        // PHY decides what goes on the wire -- preamble, sync word, header,
        // CRC, FEC, encryption. That is the boundary a DTC radio takes over.
        pending_payload.assign(aggr.begin(), aggr.end());
        pending_payload_len = aggr_payload_bytes;
        tx_frames_sealed_.fetch_add(1, std::memory_order_relaxed);
        aggr.clear();
        aggr_count = 0;
        aggr_payload_bytes = 0;
        have_pending = true;
    };

    while (running_.load()) {
        if (cfg_.arq) {
            // Drain queued ACK/control frames first — cheap, and keeps the
            // peer's retransmit timers from firing unnecessarily.
            while (auto* slot = ctrl_ring_.peek()) {
                uint32_t ack_seq = 0;
                std::memcpy(&ack_seq, slot->data, sizeof(ack_seq));
                phy_->sendFrame(nullptr, 0, FL_CTRL | FL_ACK, ack_seq);
                ctrl_ring_.consume();
            }
            // Retransmit anything whose ACK timeout elapsed. The window holds
            // PAYLOAD now, so a retransmit is re-framed at the current
            // modulation rather than replaying stale wire bytes -- which is
            // what link adaptation needs it to do.
            for (auto& pf : arq_->pollTimeouts()) {
                uint8_t f = FL_AGGR;
                if (cfg_.fec)     f |= FL_FEC;
                if (cfg_.encrypt) f |= FL_ENCRYPT;
                phy_->sendFrame(pf.encoded_bytes.data(), pf.encoded_bytes.size(),
                                f, pf.seq);
            }

            stats_.arq_acked      .store(arq_->acked(),       std::memory_order_relaxed);
            stats_.arq_retransmits.store(arq_->retransmits(), std::memory_order_relaxed);
            stats_.arq_dropped    .store(arq_->dropped(),     std::memory_order_relaxed);
        }

        if (!have_pending) {
            ssize_t n = 0;
            { StageProfiler::Scope sc(prof_, StageProfiler::TX_TAPR);
              // Block on the queue rather than poll it.
              //
              // This used to poll and then sleep 1 ms whenever the queue was
              // momentarily empty -- 12,855 times in a 25 s run, ~12.8 s of
              // sleeping. The reader thread kept filling during every sleep,
              // so the queue overflowed (8,415 drops) while the worker sat
              // idle. High empty-polls and high drops were the same bug seen
              // from both ends.
              //
              // The wait is bounded so a stage holding data still gets
              // flushed on its latency deadline when traffic stops.
              std::unique_lock<std::mutex> lk(tap_mu_);
              if (tap_queue_.empty())
                  // Wait long enough to actually sleep. The reader notifies on
                  // every packet, so a short timeout adds no responsiveness --
                  // it just spins: a 250us bound produced 50,719 polls to
                  // collect 5,174 packets, burning 13.95 s of a 25 s run in
                  // the queue read alone, more than all the DSP combined. The
                  // bound only has to be shorter than the flush deadline so a
                  // partially-filled stage still goes out on a quiet link.
                  tap_cv_.wait_for(lk, std::chrono::milliseconds(TX_LATENCY_MS / 2),
                                   [&]{ return !tap_queue_.empty() || !running_.load(); });
              if (!tap_queue_.empty()) {
                  auto& front = tap_queue_.front();
                  n = static_cast<ssize_t>(front.size());
                  std::memcpy(pkt.data(), front.data(), front.size());
                  tap_queue_.pop_front();
              } }
            if (n > 0) {
                tx_tap_pkts_ .fetch_add(1, std::memory_order_relaxed);
                tx_tap_bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            } else {
                tx_tap_empty_.fetch_add(1, std::memory_order_relaxed);
            }
            if (n <= 0) {
                // TAP is empty right now. Seal whatever has accumulated once
                // it has waited long enough, then push whatever is staged.
                // Neither happens on every poll: this fires once a
                // millisecond, and flushing that often is what kept the TX
                // buffer ~10% full.
                if (!aggr.empty() &&
                    std::chrono::steady_clock::now() - aggr_started >=
                        std::chrono::milliseconds(TX_AGGREGATE_MS)) {
                    sealAggregate();
                } else {
                    // Nothing left to aggregate: tell the PHY it may send what
                    // it is holding. Only the daemon knows the offered traffic
                    // has paused, so only the daemon can say so. No sleep here
                    // -- the wait above already blocks on the queue.
                    phy_->flushPending();
                    continue;
                }
            } else {
                // Append [u16 len][packet]. Seal first if this packet would
                // push the aggregate past the target, so frames land near the
                // efficient size rather than overshooting it.
                const size_t need = aggregate::costOf(static_cast<size_t>(n));
                if (!aggr.empty() && aggr.size() + need > AGGR_TARGET_BYTES)
                    sealAggregate();

                if (aggr.empty()) aggr_started = std::chrono::steady_clock::now();
                aggregate::append(aggr, pkt.data(), static_cast<size_t>(n));
                ++aggr_count;
                aggr_payload_bytes += static_cast<size_t>(n);

                // Full enough that another typical packet would not fit.
                if (!have_pending && aggr.size() >= AGGR_TARGET_BYTES - 64)
                    sealAggregate();
                if (!have_pending) continue;   // keep filling
            }
        }

        if (cfg_.arq && !arq_->trySend(pending_seq, pending_payload)) {
            tx_arq_blocked_.fetch_add(1, std::memory_order_relaxed);
            continue; // window full — hold the packet, retry next loop
        }

        // Backpressure is a refusal, not an error: hold the frame and retry
        // rather than dropping it here, so loss stays visible at the TAP
        // queue where it is counted.
        if (!phy_->sendFrame(pending_payload.data(), pending_payload.size(),
                             pending_flags, pending_seq)) {
            if (!running_.load()) break;
            continue;
        }
        stats_.bytes_tx .fetch_add(static_cast<uint64_t>(pending_payload_len),
                                   std::memory_order_relaxed);
        have_pending = false;
    }

}

void BridgeMode::statThread() {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto last  = start;
    uint64_t prev_tx = 0, prev_rx = 0;
    int temp_tick = 0;
    double prev_air = 0.0;
    int duty_ticks = 0;

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

        // delta divided by elapsed time is the real occupancy the limiter is
        // meant to cap -- and the one the peer's burst detector experiences.
        {
            const double air = phy_ ? phy_->stats().air_seconds : 0.0;
            double d   = (dt > 0) ? 100.0 * (air - prev_air) / dt : 0.0;
            prev_air = air;
            if (d < 0) d = 0;
            stats_.tx_duty_now.store(static_cast<float>(d), std::memory_order_relaxed);
            // Peak ignores the first few ticks: nothing is queued yet at
            // startup and the ratio is meaningless there.
            if (++duty_ticks > 5 &&
                d > stats_.tx_duty_peak.load(std::memory_order_relaxed))
                stats_.tx_duty_peak.store(static_cast<float>(d), std::memory_order_relaxed);
        }

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
