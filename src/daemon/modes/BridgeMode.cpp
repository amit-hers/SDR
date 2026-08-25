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
    std::cerr << "[sdr] rx buffer " << cfg_.rx_buffer_samples << " samples ("
              << (cfg_.rx_buffer_samples /
                  (cfg_.bw_mhz * cfg_.samples_per_symbol * 1.0e6) * 1000.0)
              << " ms deadline), queue depth " << cfg_.rx_queue_depth << "\n";
    std::cerr << "[sdr] carrier sense: "
              << (cfg_.carrier_sense ? "ON" : "OFF")
              << " (hold " << cfg_.carrier_sense_hold_ms << " ms, max defer "
              << cfg_.carrier_sense_max_defer_ms << " ms)\n";
    if (cfg_.tx_duty_max > 0.0 && cfg_.tx_duty_max < 1.0)
        std::cerr << "[sdr] TX duty limit: " << (cfg_.tx_duty_max * 100.0)
                  << "% (gaps inserted between bursts so the peer can acquire)\n";
    else
        std::cerr << "[sdr] TX duty limit: DISABLED -- the channel may saturate\n";
    std::cerr << "[sdr] RX carrier recovery: "
              << (carrier_mode_ == CarrierMode::LS        ? "LS (data-aided, open loop)"
                : carrier_mode_ == CarrierMode::COSTAS    ? "Costas loop over whole burst"
                : carrier_mode_ == CarrierMode::LS_COSTAS ? "LS derotation + Costas on payload only"
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
    tap_reader_thread_ = std::thread(&BridgeMode::tapReaderThread, this);
    tx_pusher_thread_  = std::thread(&BridgeMode::txPusherThread,  this);
    tx_thread_      = std::thread(&BridgeMode::txThread,      this);
    capture_thread_ = std::thread(&BridgeMode::captureThread, this);
    rx_thread_      = std::thread(&BridgeMode::rxThread,      this);
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
    capture_cv_.notify_all();   // wake the DSP thread out of its wait
    tap_cv_.notify_all();       // wake the TX worker off the TAP queue
    tx_sq_cv_.notify_all();     // wake modulator and pusher off the sample queue
    if (tx_thread_.joinable())      tx_thread_.join();
    if (tap_reader_thread_.joinable()) tap_reader_thread_.join();
    tx_sq_cv_.notify_all();
    if (tx_pusher_thread_.joinable())  tx_pusher_thread_.join();
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
        auto tx = radio_.txStats();
        std::cerr << "tx airtime=" << tx_air_seconds_.load() << " s over "
                  << wall << " s wall = "
                  << (wall > 0 ? 100.0 * tx_air_seconds_.load() / wall : 0.0)
                  << "% duty (limit " << cfg_.tx_duty_max * 100.0 << "%)\n";
        std::cerr << "tx pushes=" << tx.pushes
                  << " requested=" << tx.requested_pairs
                  << " pushed=" << tx.pushed_pairs
                  << " (" << (tx.requested_pairs ? 100.0 * double(tx.pushed_pairs)
                                                 / double(tx.requested_pairs) : 0.0)
                  << "% of requested samples reached the radio)"
                  << " short_pushes=" << tx.short_pushes
                  << " | frames_tx(on air)=" << stats_.frames_tx.load()
                  << " frames_lost_in_push=" << tx_frames_lost_.load() << "\n";
        std::cerr << "carrier sense: defers=" << cs_defers_.load()
                  << " forced_overrides=" << cs_overrides_.load() << "\n";
        // TX pipeline, stage by stage. Read left to right: each stage should
        // account for the one before it. Whichever column collapses first is
        // where offered load is being shed.
        std::cerr << "TX-PIPE tap_pkts=" << tx_tap_pkts_.load()
                  << " tap_bytes=" << tx_tap_bytes_.load()
                  << " tap_empty_polls=" << tx_tap_empty_.load()
                  << " -> sealed=" << tx_frames_sealed_.load()
                  << " -> staged=" << tx_frames_staged_.load()
                  << " -> pushes_ok=" << tx_pushes_ok_.load()
                  << " pushes_short=" << tx_pushes_short_.load() << "\n";
        {
            uint64_t it = walk_iters_.load(), sf = walk_sync_found_.load();
            uint64_t fo = walk_frames_ok_.load(), win = stats_.bursts_detected.load();
            std::cerr << "RX-WALK iters=" << it
                      << " sync_found=" << sf
                      << " (" << (it ? 100.0 * double(sf) / double(it) : 0.0) << "% of iters)"
                      << " frames_ok=" << fo
                      << " frames/window=" << (win ? double(fo) / double(win) : 0.0)
                      << " adv_past_frame=" << walk_adv_frame_.load()
                      << " adv_by_template=" << walk_adv_ref_.load() << "\n";
            std::cerr << "RX-WALK exits: no_sync=" << walk_exit_nosync_.load()
                      << " end_of_window=" << walk_exit_eow_.load()
                      << " hit_max_frames=" << walk_exit_maxframes_.load()
                      << " (cap=" << MAX_FRAMES_PER_WINDOW << ")\n";
        }
        {
            uint64_t n = q_n_.load(), en = q_evm_n_.load();
            std::cerr << "RX-QUAL das_n=" << n
                      << " |resid_cfo|=" << (n ? double(q_freq_abs_ur_.load()) / double(n) / 1e6 : 0.0)
                      << " rad/sym"
                      << " das_quality=" << (n ? double(q_qual_milli_.load()) / double(n) / 1000.0 : 0.0)
                      << " | EVM acq=" << (en ? double(q_evm_acq_pct_.load()) / double(en) : 0.0) << "%"
                      << " payload=" << (en ? double(q_evm_pay_pct_.load()) / double(en) : 0.0) << "%"
                      << " (n=" << en << ")\n";
            uint64_t pn = q_pass_n_.load(), fn = q_fail_n_.load();
            auto avg = [](uint64_t sum, uint64_t k){ return k ? double(sum)/double(k) : 0.0; };
            std::cerr << "RX-QUAL  CRC-PASS n=" << pn
                      << " EVM acq=" << avg(q_pass_acq_.load(), pn) << "%"
                      << " pay=" << avg(q_pass_pay_.load(), pn) << "%"
                      << " |cfo|=" << avg(q_pass_cfo_ur_.load(), pn)/1e6 << " rad/sym\n";
            auto pc=[&](uint64_t v,uint64_t k){ return k ? 100.0*double(v)/double(k) : 0.0; };
            std::cerr << "RX-STRUCT PASS n=" << pn
                      << " mean_plen=" << avg(st_pass_plen_.load(), pn)
                      << " inverted=" << pc(st_pass_inv_.load(), pn) << "%"
                      << " short_bytes=" << pc(st_pass_short_.load(), pn) << "%"
                      << " aggr=" << pc(st_pass_aggr_.load(), pn) << "%\n";
            std::cerr << "RX-STRUCT FAIL n=" << fn
                      << " mean_plen=" << avg(st_fail_plen_.load(), fn)
                      << " inverted=" << pc(st_fail_inv_.load(), fn) << "%"
                      << " short_bytes=" << pc(st_fail_short_.load(), fn) << "%"
                      << " aggr=" << pc(st_fail_aggr_.load(), fn) << "%"
                      << " wrong_mod=" << pc(st_fail_modbad_.load(), fn) << "%\n";
            uint64_t tp = t_pass_n_.load(), tf = t_fail_n_.load();
            std::cerr << "RX-TAIL  PASS n=" << tp
                      << " p95=" << avg(t_pass_p95_.load(), tp) << "%"
                      << " p99=" << avg(t_pass_p99_.load(), tp) << "%"
                      << " max=" << avg(t_pass_max_.load(), tp) << "%"
                      << " weak_syms/frame=" << avg(t_pass_weak_.load(), tp) << "\n";
            std::cerr << "RX-TAIL  FAIL n=" << tf
                      << " p95=" << avg(t_fail_p95_.load(), tf) << "%"
                      << " p99=" << avg(t_fail_p99_.load(), tf) << "%"
                      << " max=" << avg(t_fail_max_.load(), tf) << "%"
                      << " weak_syms/frame=" << avg(t_fail_weak_.load(), tf) << "\n";
            {
                uint64_t cn = cs_probe_n_.load();
                auto a=[&](std::atomic<uint64_t>& x){ return cn ? double(x.load())/double(cn)/1000.0 : 0.0; };
                std::cerr << "RX-COSTAS n=" << cn
                          << " | amp q1/rest  in=" << a(cs_in_amp_ratio_)
                          << " out=" << a(cs_out_amp_ratio_)
                          << " | phase_err rad  in q1=" << a(cs_in_ph_q1_)
                          << " rest=" << a(cs_in_ph_rest_)
                          << "  out q1=" << a(cs_out_ph_q1_)
                          << " rest=" << a(cs_out_ph_rest_) << "\n";
            }
            std::cerr << "RX-LOC   PASS worst_pos=" << avg(t_pass_wpos_.load(), tp)/1000.0
                      << " weak_first_q=" << avg(t_pass_wfq_.load(), tp)
                      << " weak_last_q=" << avg(t_pass_wlq_.load(), tp) << "\n";
            std::cerr << "RX-LOC   FAIL worst_pos=" << avg(t_fail_wpos_.load(), tf)/1000.0
                      << " weak_first_q=" << avg(t_fail_wfq_.load(), tf)
                      << " weak_last_q=" << avg(t_fail_wlq_.load(), tf) << "\n";
            std::cerr << "RX-QUAL  CRC-FAIL n=" << fn
                      << " EVM acq=" << avg(q_fail_acq_.load(), fn) << "%"
                      << " pay=" << avg(q_fail_pay_.load(), fn) << "%"
                      << " |cfo|=" << avg(q_fail_cfo_ur_.load(), fn)/1e6 << " rad/sym\n";
        }
        {
            auto tx = radio_.txStats();
            double per = tx.pushes ? double(tx.pushed_pairs) / double(tx.pushes) : 0.0;
            std::cerr << "TX-BATCH pushes=" << tx.pushes
                      << " samples/push=" << per
                      << " (" << (radio_.txCapacity() ? 100.0*per/double(radio_.txCapacity()) : 0.0)
                      << "% of buffer)"
                      << " empty_polls=" << tx_tap_empty_.load() << "\n";
        }
        std::cerr << "TX-PIPELINE sample_q_stalls=" << tx_sq_stalls_.load()
                  << " (modulator waiting on the pusher; low = overlap working)\n";
        std::cerr << "TX-QUEUE drops=" << tap_q_drops_.load()
                  << " high_water=" << tap_q_hiwater_.load()
                  << " / " << TAP_QUEUE_MAX << "\n";
        std::cerr << "TX-BLOCK duty_defers=" << tx_duty_defers_.load()
                  << " duty_slept=" << (tx_duty_waits_us_.load() / 1e6) << " s"
                  << " (" << (wall > 0 ? 100.0 * (tx_duty_waits_us_.load() / 1e6) / wall : 0.0)
                  << "% of wall)"
                  << " cs_defers=" << cs_defers_.load()
                  << " stage_forced_flushes=" << tx_stage_forces_.load()
                  << " arq_blocked=" << tx_arq_blocked_.load() << "\n";
        std::cerr << "aggregation rx: records=" << rx_records_.load()
                  << " written=" << rx_rec_written_.load()
                  << " write_failed=" << rx_rec_failed_.load() << "\n";
        uint64_t h0 = align_hits_[0].load(), h1 = align_hits_[1].load(),
                 h2 = align_hits_[2].load();
        uint64_t tot = h0 + h1 + h2;
        std::cerr << "alignment retry: attempts=" << align_attempts_.load()
                  << " decodes at base=" << h0 << " base-1=" << h1
                  << " base+1=" << h2
                  << "  (neighbours contributed "
                  << (tot ? 100.0 * double(h1 + h2) / double(tot) : 0.0)
                  << "% of decodes)\n";
    }
}

// Applies SCHED_FIFO and (optionally) a core pin to the calling thread.
// Failures are reported once and are not fatal: the daemon must still run
// unprivileged, just without the jitter guarantees.
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
void BridgeMode::txPusherThread() {
    applyRealtime("txpush", 3);
    const double sample_rate_tx = static_cast<double>(cfg_.bw_mhz) * 1e6 *
                                  cfg_.samples_per_symbol;
    const double duty = (cfg_.tx_duty_max > 0.0 && cfg_.tx_duty_max < 1.0)
                      ? cfg_.tx_duty_max : 1.0;
    std::chrono::steady_clock::time_point next_tx_allowed{}, air_clock{};

    while (running_.load()) {
        std::vector<int16_t> buf;
        size_t frames = 0;
        {
            std::unique_lock<std::mutex> lk(tx_sq_mu_);
            tx_sq_cv_.wait_for(lk, std::chrono::milliseconds(5), [&]{
                return !tx_sample_q_.empty() || !running_.load(); });
            if (tx_sample_q_.empty()) continue;
            buf = std::move(tx_sample_q_.front()); tx_sample_q_.pop_front();
            frames = tx_sample_frames_.front();    tx_sample_frames_.pop_front();
        }
        tx_sq_cv_.notify_one();          // room freed for the modulator
        if (buf.empty()) continue;

        auto now = std::chrono::steady_clock::now();
        if (now < next_tx_allowed) {
            tx_duty_waits_us_.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    next_tx_allowed - now).count()), std::memory_order_relaxed);
            std::this_thread::sleep_for(next_tx_allowed - now);
        }
        if (cfg_.carrier_sense) {
            now = std::chrono::steady_clock::now();
            const auto busy_until = std::chrono::steady_clock::time_point(
                std::chrono::microseconds(peer_busy_until_us_.load(std::memory_order_relaxed)));
            if (now < busy_until) {
                auto wait = busy_until - now;
                const auto cap = std::chrono::milliseconds(cfg_.carrier_sense_max_defer_ms);
                if (wait > cap) { wait = cap; cs_overrides_.fetch_add(1, std::memory_order_relaxed); }
                std::this_thread::sleep_for(wait);
            }
        }

        const size_t want = buf.size() / 2;
        int pushed = 0;
        { StageProfiler::Scope sc(prof_, StageProfiler::TX_PUSH, want);
          pushed = radio_.txPush(buf.data(), want); }

        const double air = static_cast<double>(want) / sample_rate_tx;
        tx_air_seconds_.store(tx_air_seconds_.load(std::memory_order_relaxed) + air,
                              std::memory_order_relaxed);
        auto after_push = std::chrono::steady_clock::now();
        if (air_clock < after_push) air_clock = after_push;
        air_clock += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(air));
        next_tx_allowed = air_clock +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(air / duty - air));

        if (pushed >= 0 && static_cast<size_t>(pushed) == want) {
            tx_pushes_ok_.fetch_add(1, std::memory_order_relaxed);
            stats_.frames_tx.fetch_add(frames, std::memory_order_relaxed);
        } else {
            tx_pushes_short_.fetch_add(1, std::memory_order_relaxed);
            tx_frames_lost_.fetch_add(frames, std::memory_order_relaxed);
        }
    }
}

// Continuously drains the TAP into tap_queue_. Runs independently of the
// transmit worker so duty-limit sleeps never stall ingestion.
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
    if (const char* pth = std::getenv("SDR_TXDUMP"))
        tx_dump_.open(pth, std::ios::binary | std::ios::trunc);
    std::vector<uint8_t>             pkt(MAX_PAYLOAD);
    std::vector<std::complex<float>> iq_syms, iq_shaped;
    std::vector<int16_t>             iq_hw;
    RRCInterp interp(cfg_.samples_per_symbol);

    // A txPush costs the full tx-buffer length in airtime no matter how few
    // samples were written, so frames are staged here and flushed together.
    // Without this, a short frame wastes >98% of the air time on zero padding
    // and the link tops out around 15 frames/sec regardless of symbol rate.
    const size_t tx_capacity = radio_.txCapacity();
    std::vector<int16_t> stage;
    stage.reserve(tx_capacity * 2);

    // Frames currently sitting in `stage`, i.e. modulated but not yet handed
    // to the radio. They only count as transmitted once a push completes.
    size_t staged_frames = 0;

    // Transmit pacing. After sending `air` seconds of samples the node stays
    // quiet until `air / duty` has elapsed, so a gap of air*(1/duty - 1)
    // opens between bursts. Without this the transmitter fills the channel
    // solid and the peer's burst detector has nothing to acquire in.
    const double sample_rate_tx = static_cast<double>(cfg_.bw_mhz) * 1e6 *
                                  cfg_.samples_per_symbol;
    const double duty = (cfg_.tx_duty_max > 0.0 && cfg_.tx_duty_max < 1.0)
                      ? cfg_.tx_duty_max : 1.0;
    std::chrono::steady_clock::time_point next_tx_allowed{};

    // When everything pushed so far will have finished being radiated.
    //
    // The gap used to be measured from the moment txPush RETURNED, but that
    // call hands samples to a DMA buffer and returns immediately -- long
    // before the radio has put them on the air. The gap was therefore timed
    // against work that had not happened yet, and the cap did not bind:
    // measured peak duty 125.6% against a configured 65%, i.e. more airtime
    // committed per tick than there is time in the tick. The transmitter
    // filled the channel solid and the peer's burst detector had no gaps to
    // acquire in (acquisition fell 85% -> 31% under load).
    //
    // Pacing against this air clock instead makes the gap start where the
    // burst actually ends, so the duty cap holds regardless of how deeply
    // the radio buffers. When idle it catches up to now, so quiet periods
    // are not banked as credit for a later over-long burst.
    std::chrono::steady_clock::time_point air_clock{};

    // `force` waits out the gap instead of deferring -- used when the stage
    // is full and deferring would overflow it.
    auto flush = [&](bool force) {
        if (stage.empty()) return;
        // Hand the modulated buffer to the pusher thread instead of pushing
        // it here. txPush blocks until the radio accepts the samples -- 50.3%
        // of this worker's wall time -- and every microsecond of that was
        // time not spent modulating the next buffer. Duty pacing and carrier
        // sense move to the pusher, where they belong: both are properties of
        // when samples reach the air, not of when they were prepared.
        std::unique_lock<std::mutex> lk(tx_sq_mu_);
        if (tx_sample_q_.size() >= TX_SAMPLE_Q_MAX) {
            if (!force) { tx_sq_stalls_.fetch_add(1, std::memory_order_relaxed); return; }
            // Bounded: a missed notify must degrade to a slow flush, never a hang.
            tx_sq_cv_.wait_for(lk, std::chrono::milliseconds(50), [&]{
                return tx_sample_q_.size() < TX_SAMPLE_Q_MAX || !running_.load(); });
            if (tx_sample_q_.size() >= TX_SAMPLE_Q_MAX) {
                tx_sq_stalls_.fetch_add(1, std::memory_order_relaxed);
                return;                       // try again on the next poll
            }
        }
        tx_sample_q_.emplace_back(std::move(stage));
        tx_sample_frames_.push_back(staged_frames);
        lk.unlock();
        tx_sq_cv_.notify_one();
        stage = std::vector<int16_t>();
        stage.reserve(tx_capacity * 2);
        staged_frames = 0;
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
        if (stage.size() / 2 + iq_shaped.size() > tx_capacity) {
            tx_stage_forces_.fetch_add(1, std::memory_order_relaxed);
            flush(/*force=*/true);
        }
        if (iq_shaped.size() > tx_capacity) {
            int pushed = radio_.txPush(iq_hw.data(), iq_shaped.size());
            if (pushed >= 0 && static_cast<size_t>(pushed) == iq_shaped.size())
                stats_.frames_tx.fetch_add(1, std::memory_order_relaxed);
            else
                tx_frames_lost_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ++staged_frames;
        tx_frames_staged_.fetch_add(1, std::memory_order_relaxed);
        if (stage.empty()) stage_started_ = std::chrono::steady_clock::now();
        stage.insert(stage.end(), iq_hw.begin(), iq_hw.end());
    };

    bool                  have_pending{false};
    uint32_t              pending_seq{0};
    size_t                pending_payload_len{0};
    std::vector<uint8_t>  pending_frame;

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
        uint8_t flags = FL_AGGR;
        if (cfg_.fec)     flags |= FL_FEC;
        if (cfg_.encrypt) flags |= FL_ENCRYPT;
        pending_seq = tx_seq_.fetch_add(1, std::memory_order_relaxed);

        Framer framer;
        StageProfiler::Scope sc_fr(prof_, StageProfiler::TX_FRAME, aggr.size());
        pending_frame = framer.encode(aggr.data(), aggr.size(),
                                      flags, tx_mod_, mhzToBw(cfg_.bw_mhz),
                                      cfg_.node_id_u32, pending_seq,
                                      fec_.get(), aes_.get());
        if (frame_log_.is_open())
            frame_log_ << "[TX] seq=" << pending_seq
                       << " payload=" << aggr.size()
                       << " packets=" << aggr_count
                       << " mod=" << modCodeName(tx_mod_)
                       << " wire=" << pending_frame.size() << "B\n";
        pending_payload_len = aggr_payload_bytes;
        tx_frames_sealed_.fetch_add(1, std::memory_order_relaxed);
        // Exactly the bytes the CRC covers: past the preamble, before the
        // postamble -- byte-for-byte what the receiver's sm.bytes holds.
        if (tx_dump_.is_open() && pending_frame.size() > PREAMBLE_LEN + POSTAMBLE_LEN) {
            const size_t body_len = pending_frame.size() - PREAMBLE_LEN - POSTAMBLE_LEN;
            dumpRecord(tx_dump_, pending_seq, pending_frame.data() + PREAMBLE_LEN, body_len);
        }
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
                    // Flush only on the latency deadline. Batching is what
                    // makes each iio push worth its ~10 ms of fixed overhead:
                    // a buffer holds ~6 frames, so flushing per frame wastes
                    // five sixths of every call. No sleep here -- the wait
                    // above already blocks on the queue.
                    if (!stage.empty() &&
                        std::chrono::steady_clock::now() - stage_started_ >=
                            std::chrono::milliseconds(TX_LATENCY_MS))
                        flush(/*force=*/false);
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

        if (cfg_.arq && !arq_->trySend(pending_seq, pending_frame)) {
            tx_arq_blocked_.fetch_add(1, std::memory_order_relaxed);
            continue; // window full — hold the packet, retry next loop
        }

        transmit(pending_frame);   // stages only; frames_tx is credited on push
        stats_.bytes_tx .fetch_add(static_cast<uint64_t>(pending_payload_len),
                                   std::memory_order_relaxed);
        have_pending = false;
    }
}

// ── Capture thread ───────────────────────────────────────────────────────────
// Nothing but rxPull -> queue. Keeping this free of DSP work is what stops
// the receiver going deaf mid-burst (see CAPTURE_QUEUE_MAX in the header).
void BridgeMode::captureThread() {
    applyRealtime("capture", 2);
    while (running_.load()) {
        std::vector<int16_t> buf(static_cast<size_t>(cfg_.rx_buffer_samples) * 2);

        auto t0 = std::chrono::steady_clock::now();
        int n;
        { StageProfiler::Scope sc(prof_, StageProfiler::RX_PULL, (uint64_t)cfg_.rx_buffer_samples);
          n = radio_.rxPull(buf.data(), static_cast<size_t>(cfg_.rx_buffer_samples)); }
        auto t1 = std::chrono::steady_clock::now();

        auto us = [](auto a, auto b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
        };
        stats_.rx_pull_us .fetch_add(us(t0, t1), std::memory_order_relaxed);
        // Span the whole iteration, measured from the END of the previous
        // one. Taking it from t0 to "now" bracketed the same code as
        // rx_pull_us and made the ratio ~1 by construction.
        {
            static thread_local std::chrono::steady_clock::time_point prev_iter{};
            auto now2 = std::chrono::steady_clock::now();
            if (prev_iter.time_since_epoch().count() != 0)
                stats_.rx_total_us.fetch_add(us(prev_iter, now2),
                                             std::memory_order_relaxed);
            prev_iter = now2;
        }

        if (n <= 0) continue;
        buf.resize(static_cast<size_t>(n) * 2);

        // Carrier sense: cheap strided energy estimate on the buffer we just
        // pulled. This runs here, not in the DSP thread, so the answer is at
        // most one buffer stale rather than a whole queue deep.
        if (cfg_.carrier_sense) {
            double acc = 0; size_t cnt = 0;
            for (size_t i = 0; i + 1 < buf.size(); i += 128) {   // ~1 in 64 samples
                double re = buf[i] / 2048.0, im = buf[i + 1] / 2048.0;
                acc += std::sqrt(re * re + im * im);
                ++cnt;
            }
            const double lvl = cnt ? acc / static_cast<double>(cnt) : 0.0;
            // Track the quietest level seen, with slow upward relaxation so a
            // permanently busy channel cannot pin the floor at a stale value.
            if (cs_noise_floor_ <= 0.0)      cs_noise_floor_ = lvl;
            else if (lvl < cs_noise_floor_)  cs_noise_floor_ = lvl;
            else                             cs_noise_floor_ += (lvl - cs_noise_floor_) * 0.0005;

            if (lvl > cs_noise_floor_ * 3.0) {
                auto until = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(cfg_.carrier_sense_hold_ms);
                peer_busy_until_us_.store(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        until.time_since_epoch()).count(),
                    std::memory_order_relaxed);
            }
        }

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
            if (capture_queue_.size() >= static_cast<size_t>(cfg_.rx_queue_depth)) {
                capture_queue_.pop_front();          // DSP can't keep up
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            capture_queue_.push_back(std::move(buf));
        }
        capture_cv_.notify_one();
    }
    capture_cv_.notify_all();   // release a waiting DSP thread on shutdown
}

// Scale-normalised EVM against the decided constellation, in percent.
// Measured on the symbols actually handed to the demodulator, so it reflects
// everything upstream: timing recovery, CFO correction and carrier tracking.
// Per-symbol error statistics against the decided constellation.
//
// The mean alone cannot see what breaks frames: one bad symbol in ~55,000
// fails an 8352-bit frame, which is a 7% frame-error rate while shifting mean
// EVM by under 2%. Frame errors live in the tail, so record p95/p99/max and
// count symbols whose decision margin is small enough to be a likely bit
// error.
struct EvmStats {
    double rms{0}, p95{0}, p99{0}, max{0};
    uint32_t weak{0};      // symbols decided with little margin
    uint32_t n{0};
    // Where the damage sits. Failures are driven by a few extreme symbols
    // (2 MHz: max error 115.9% on failed frames vs 105.5% on passed, while
    // rms differs by 2%), so the position of those outliers localises the
    // cause: clustered at the start implicates AGC/carrier settling, at the
    // end burst truncation or trailing edge, uniform implicates noise.
    double   worst_pos{0};   // position of the max-error symbol, 0..1
    uint32_t weak_first_q{0};// weak symbols in the first quarter of the frame
    uint32_t weak_last_q{0}; // ... and in the last quarter
};

// Scaling note: symbols are normalised so their MEAN MAGNITUDE matches the
// constellation's. QPSK decisions sit at (+-1,+-1), mean magnitude sqrt(2);
// BPSK at (+-1,0), mean magnitude 1. Normalising both to radius 1 (as this
// did) inflated QPSK error by sqrt(2) and made the acquisition/payload split
// incomparable -- the very comparison it existed to support.
static EvmStats evmStats(const std::vector<std::complex<float>>& s,
                         size_t a, size_t b, bool qpsk) {
    EvmStats st;
    if (b <= a || b > s.size()) return st;
    double amp = 0.0;
    for (size_t i = a; i < b; ++i) amp += std::abs(s[i]);
    amp /= double(b - a);
    if (amp <= 0.0) return st;
    const double scale = (qpsk ? std::sqrt(2.0) : 1.0) / amp;

    std::vector<double> e;
    e.reserve(b - a);
    double sum2 = 0.0;
    for (size_t i = a; i < b; ++i) {
        const double re = s[i].real() * scale, im = s[i].imag() * scale;
        const double dr = (re >= 0 ? 1.0 : -1.0);
        const double di = qpsk ? (im >= 0 ? 1.0 : -1.0) : 0.0;
        const double d2 = (re - dr) * (re - dr) + (im - di) * (im - di);
        sum2 += d2;
        e.push_back(std::sqrt(d2));
        // Decision margin: distance to the nearest decision boundary. QPSK
        // decides on both axes, BPSK only on the real one.
        const double margin = qpsk ? std::min(std::fabs(re), std::fabs(im))
                                   : std::fabs(re);
        if (margin < 0.35) {
            ++st.weak;
            const double frac = double(i - a) / double(b - a);
            if (frac < 0.25) ++st.weak_first_q;
            else if (frac >= 0.75) ++st.weak_last_q;
        }
    }
    st.n   = static_cast<uint32_t>(e.size());
    st.rms = 100.0 * std::sqrt(sum2 / double(e.size()));
    std::sort(e.begin(), e.end());
    auto pick = [&](double q) {
        size_t k = static_cast<size_t>(q * double(e.size() - 1));
        return 100.0 * e[k];
    };
    st.p95 = pick(0.95);
    st.p99 = pick(0.99);
    st.max = 100.0 * e.back();
    // Locate the worst symbol in the unsorted sequence.
    {
        double worst = -1.0; size_t wi = a;
        for (size_t i = a; i < b; ++i) {
            const double re = s[i].real() * scale, im = s[i].imag() * scale;
            const double dr = (re >= 0 ? 1.0 : -1.0);
            const double di = qpsk ? (im >= 0 ? 1.0 : -1.0) : 0.0;
            const double d2 = (re - dr) * (re - dr) + (im - di) * (im - di);
            if (d2 > worst) { worst = d2; wi = i; }
        }
        st.worst_pos = double(wi - a) / double(b - a);
    }
    return st;
}

static double evmPct(const std::vector<std::complex<float>>& s,
                     size_t a, size_t b, bool qpsk) {
    return evmStats(s, a, b, qpsk).rms;
}

// ── RX thread ────────────────────────────────────────────────────────────────
void BridgeMode::rxThread() {
    applyRealtime("dsp", 3);
    if (const char* pth = std::getenv("SDR_RXFAIL"))
        rx_fail_dump_.open(pth, std::ios::binary | std::ios::trunc);
    std::vector<int16_t>             iq_hw;
    std::vector<std::complex<float>> iq_f, window_buf, offset_buf, iq_timed, iq_syms;
    AGC agc; TimingSync tsync(cfg_.samples_per_symbol);
    // Costas bandwidth. The default 0.04 is far wider than this path needs:
    // DataAidedSync::derotate has already removed the bulk phase and
    // frequency, leaving ~0.002 rad/sym of residual to track. A wide loop
    // with almost nothing to follow is slip-prone, and a quarter-cycle slip
    // is catastrophic here -- it rotates every remaining symbol by 90 deg,
    // which under Gray coding flips one bit per symbol and corrupts the frame
    // from the slip point to the end. Measured at 2 MHz: 86% of corrupted
    // bytes carry a one-bit-per-symbol-pair XOR mask (16 of 256 possible;
    // 6% by chance), in runs of 800-1048 bytes.
    // 0.004, not liquid's 0.04 default. Swept live at 2 MHz:
    //   0.04   -> 187 bad frames, CRC 95.0%
    //   0.01   -> 146 bad,        CRC 95.5%
    //   0.004  -> 105 bad,        CRC 96.8%   <- knee
    //   0.0015 -> 113 bad,        CRC 96.5%   (too slow to track)
    // Confirmed on a second sweep: 73 bad / CRC 96.5% at 0.004.
    //
    // Both stages are needed -- measured at 2 MHz, bw 0.004:
    //   ls+costas  CRC 96.5%   494 kbps
    //   ls only    CRC 17.3%    53 kbps   (untracked drift is fatal)
    //   costas only CRC 39.8%  210 kbps   (no acquisition without the LS estimate)
    float costas_bw = 0.004f;
    if (const char* e = std::getenv("SDR_COSTAS_BW")) {
        float v = std::strtof(e, nullptr);
        if (v > 0.f && v < 1.f) costas_bw = v;
    }
    // Quadrant-slip guard: DISABLED (0).
    //
    // The idea was sound, the reference frame was not. setPhaseLimit() bounds
    // phase against a FIXED start, but the loop must legitimately ramp to
    // follow residual frequency: even 2e-4 rad/sym accumulates 0.84 rad over
    // a 4192-symbol payload, past pi/4. The clamp therefore cannot tell a
    // slip from normal tracking, and it blocks the tracking.
    //
    // Measured at 2 MHz, monotonically worse as it tightens:
    //   off    3728/149  CRC 96.2%  891.8 kbps
    //   pi/4   1147/1317 CRC 46.6%  224.6 kbps
    //   0.5     759/1245 CRC 37.9%  136.1 kbps
    //   0.35    528/1197 CRC 30.6%   88.6 kbps
    //
    // A correct version would bound DETRENDED phase -- deviation from the
    // loop's own frequency-predicted ramp -- which can distinguish the two.
    // Not built, because three separate approaches (narrower bandwidth,
    // residual seeding, this clamp) all reduced or removed slips and only
    // bandwidth improved CRC. At ~3% frame loss the slips are no longer the
    // binding constraint.
    costas_phase_limit_ = 0.f;
    if (const char* e = std::getenv("SDR_COSTAS_PHLIM"))
        costas_phase_limit_ = std::strtof(e, nullptr);
    costas_seed_ = 0;
    if (const char* e = std::getenv("SDR_COSTAS_SEED")) {
        std::string v(e);
        if      (v == "freq")  costas_seed_ = 1;
        else if (v == "resid") costas_seed_ = 2;
    }
    std::cerr << "[rx] costas loop bandwidth: " << costas_bw
              << " seed=" << (costas_seed_==1?"freq":costas_seed_==2?"resid":"none")
              << " phase_limit=" << costas_phase_limit_ << "\n";
    CostasLoop costas(costas_bw);

    // Free-running fixed-point timing recovery (SDR_TSYNC=fixed).
    //
    // The windowed path resets the timing loop at every burst, so it
    // re-acquires from cold each time. Measured on recorded captures, letting
    // the loop run continuously instead lifts CRC 81.4% -> 96.9% on 1242-byte
    // payloads and recovers 37% more complete frames, because the loop is
    // already locked when a burst arrives. It is also 3.2x faster than
    // symsync_crcf (20.9 vs 6.56 Msamp/s), which is why it can afford to run
    // over every sample rather than only over detected bursts.
    //
    // This is the same arrangement the block would have in fabric: no CFO and
    // no AGC ahead of it, never reset. CFO moves to the symbol domain, where
    // it still helps (measured: symbol-rate CFO beat Costas-alone on all
    // three captures), and AGC is dropped -- the Gardner error is
    // power-normalised, so it does not need one.
    // SDR_TSYNC selects the timing-recovery implementation:
    //   liquid            symsync_crcf, the previous default. Measured on a
    //                     clean coax link at 52-58% CRC where the fixed path
    //                     managed 99%, on near-identical channels seconds
    //                     apart (846 vs 842 bursts). It holds up over the air
    //                     -- 98.8% CRC earlier -- so it degrades specifically
    //                     with a strong, non-fading signal.
    //   fixed   (default) FixedTimingSync as a DROP-IN inside the existing
    //                     per-burst pipeline -- CFO, AGC, per-burst reset and
    //                     preamble alignment all unchanged, only the matched
    //                     filter + symbol timing swapped. This is where the
    //                     measured ~5x speedup (34 vs 6.9 Msamp/s on short
    //                     calls) pays off without disturbing anything that
    //                     works. Live A/B, two alternating trials at matched
    //                     offered load: CRC 99.3% vs 55.3%, 1.9x the frames,
    //                     acquisition 90.7% vs 51.2%, and CPU 31% vs 38.5%.
    //                     Better on reliability AND cost at the same time.
    //   freerun           the continuous, never-reset architecture. Measured
    //                     live at 19-20% burst acquisition against liquid's
    //                     69-72%, so it is NOT the default; kept for further
    //                     investigation of why the offline replay disagreed.
    const std::string tsync_sel = [] {
        const char* e = std::getenv("SDR_TSYNC");
        return std::string(e ? e : "fixed");
    }();
    const bool tsync_freerun = (tsync_sel == "freerun");
    const bool tsync_dropin  = (tsync_sel == "fixed");
    FixedTimingSync::Config ftcfg;
    ftcfg.sps      = cfg_.samples_per_symbol;
    ftcfg.interp   = FixedTimingSync::Config::Interp::POLYPHASE;
    ftcfg.n_phases = 32;
    ftcfg.fixed_loop = true;
    ftcfg.alpha_sh = 12;
    ftcfg.beta_sh  = 22;
    // Overridable for sweeps. The loop locks with mu pinned at 0.94-0.99,
    // hard against the wrap boundary, so noise tips it across constantly:
    // steps of 3 and 5 input samples at ~0.55% each, and the imbalance
    // (285 threes vs 280 fives at 2 MHz) inserts symbols. Seven inserted
    // symbols shift the bit stream by 14 bits and everything after decodes
    // as garbage -- the tail matches the transmitted data at 100% once
    // realigned, so nothing is corrupted, only displaced.
    //
    // alpha_sh is the proportional gain as a right-shift, so LARGER means a
    // slower, less noise-driven loop. n_phases sets interpolation resolution
    // (32 phases = 1/32 sample); coarse resolution keeps mu quantised near
    // the boundary it is dithering across.
    if (const char* e = std::getenv("SDR_ALPHA_SH")) ftcfg.alpha_sh = std::atoi(e);
    if (const char* e = std::getenv("SDR_BETA_SH"))  ftcfg.beta_sh  = std::atoi(e);
    if (const char* e = std::getenv("SDR_NPHASES"))  ftcfg.n_phases = std::atoi(e);
    std::cerr << "[rx] timing loop: alpha_sh=" << ftcfg.alpha_sh
              << " beta_sh=" << ftcfg.beta_sh
              << " n_phases=" << ftcfg.n_phases << "\n";
    const bool slip_tracing = (std::getenv("SDR_SLIPTRACE") != nullptr);
    ftcfg.trace = slip_tracing;   // 20 vectors/symbol -- diagnosis only
    if (slip_tracing)
        slip_trace_.open(std::getenv("SDR_SLIPTRACE"), std::ios::trunc);
    FixedTimingSync fts(ftcfg);
    FixedTimingSync::Result fres;
    const double symbol_rate = static_cast<double>(cfg_.bw_mhz) * 1e6;
    if (tsync_freerun)
        std::cerr << "[rx] timing recovery: FIXED-POINT free-running "
                     "(symbol-domain CFO, no AGC)\n";
    else if (tsync_dropin)
        std::cerr << "[rx] timing recovery: FIXED-POINT drop-in (default; "
                     "SDR_TSYNC=liquid for symsync_crcf)\n";
    else
        std::cerr << "[rx] timing recovery: liquid symsync_crcf\n";
    BurstDetector::Config bcfg;
    bcfg.block_size  = static_cast<size_t>(cfg_.burst_block);
    bcfg.threshold_x = static_cast<float>(cfg_.burst_threshold);
    bcfg.margin      = static_cast<size_t>(cfg_.burst_margin);
    bcfg.merge_gap   = static_cast<size_t>(cfg_.burst_merge_gap);
    bcfg.noise_quantile = static_cast<float>(cfg_.burst_noise_q);
    BurstDetector detector(bcfg);
    PreambleSync  psync(cfg_.samples_per_symbol);
    // Symbol-rate twin of psync, for the free-running path. At 1 sample per
    // symbol the RRC-shaped reference collapses to the preamble symbols
    // themselves, which is the correct matched reference post-decimation.
    PreambleSync  psym(1);
    DataAidedSync dasync;
    uint64_t logged_crc_errors = 0;
    const double sample_rate = static_cast<double>(cfg_.bw_mhz) * 4e6;
    // The detector keeps burst_margin samples of context before the burst,
    // so the frame start is within roughly that plus a block. Bounding the
    // correlation search keeps its cost proportional to that, not to the
    // whole window.
    const size_t preamble_search =
        static_cast<size_t>(cfg_.burst_margin) + static_cast<size_t>(cfg_.burst_block) * 4;


    // Frame delivery, shared by both timing paths.
    //
    // Extracted verbatim from the windowed path so the free-running
    // fixed-point path cannot accidentally measure a DIFFERENT decoder.
    // The A/B is meant to isolate timing recovery; anything downstream of
    // SplitModem must be byte-identical between the two.
    bool ok_any = false;
    auto deliver = [&](const SplitModem::Result& sm, float rssi, float snr,
                       size_t oi) -> bool {
        bool this_frame_ok = false;
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
                    // Seq lives at body offset 12 (big-endian) and is
                    // readable even when the CRC fails, so failures can be
                    // matched against the transmitted frame.
                    // NCO state over the symbols this frame occupied. The
                    // byte diff shows failures are whole-byte garbage from a
                    // random mid-frame point onward (91.5% corrupt after the
                    // first difference at 2 MHz), which is a symbol timing
                    // slip rather than bit errors -- so dump mu, the timing
                    // error, the loop integrator and the per-symbol step to
                    // find the update where the loop jumps.
                    if (false) {
                        const uint8_t* b = sm.bytes.data();
                        uint32_t sq = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16)
                                    | (uint32_t(b[14]) <<  8) |  uint32_t(b[15]);
                        const size_t n = last_fts_.mu.size();
                        const size_t s0 = sm.sync_sym;
                        const size_t s1 = std::min(n, sm.end_sym ? sm.end_sym : n);
                        slip_trace_ << "FRAME seq=" << sq << " sync_sym=" << s0
                                    << " end_sym=" << s1 << " nsym=" << n << "\n";
                        slip_trace_ << "# k idx mu e_q freq_after step_pos d_idx carrier_dphi\n";
                        // Seed from the symbol BEFORE the frame, or the
                        // first row reports a self-difference of zero and
                        // looks like a duplicated symbol. That artifact cost
                        // a wrong root-cause diagnosis once already.
                        long prev_idx = (s0 > 0 && s0 <= n)
                                      ? long(last_fts_.idx[s0 - 1])
                                      : (s0 < n ? long(last_fts_.idx[s0]) - int(cfg_.samples_per_symbol) : 0);
                        for (size_t k = s0; k < s1 && k < n; ++k) {
                            long didx = long(last_fts_.idx[k]) - prev_idx;
                            prev_idx = last_fts_.idx[k];
                            slip_trace_ << (k - s0) << ' ' << last_fts_.idx[k] << ' '
                                        << last_fts_.mu[k] << ' '
                                        << (k < last_fts_.e_q.size() ? last_fts_.e_q[k] : 0) << ' '
                                        << (k < last_fts_.freq_after.size() ? last_fts_.freq_after[k] : 0) << ' '
                                        << (k < last_fts_.pos_after.size() ? last_fts_.pos_after[k] : 0) << ' '
                                        << didx << ' ';
                            // Change in applied carrier phase between
                            // consecutive payload symbols. A cycle slip shows
                            // as a step near +-pi/2 (1.571 rad).
                            {
                                const size_t pi_ = (k >= s0) ? (k - s0) : 0;
                                float dphi = 0.f;
                                if (pi_ > 0 && pi_ < last_carrier_phase_.size()) {
                                    float a = last_carrier_phase_[pi_];
                                    float b = last_carrier_phase_[pi_ - 1];
                                    dphi = a - b;
                                    while (dphi >  3.14159265f) dphi -= 6.28318531f;
                                    while (dphi < -3.14159265f) dphi += 6.28318531f;
                                }
                                slip_trace_ << dphi;
                            }
                            slip_trace_ << '\n';
                        }
                        ++slip_frames_dumped_;
                    }
                    if (rx_fail_dump_.is_open() && sm.bytes.size() >= 16) {
                        const uint8_t* b = sm.bytes.data();
                        uint32_t rseq = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16)
                                      | (uint32_t(b[14]) <<  8) |  uint32_t(b[15]);
                        dumpRecord(rx_fail_dump_, rseq, sm.bytes.data(), sm.bytes.size());
                    }
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
                ok_any = true;
                if (!this_frame_ok)
                align_hits_[oi].fetch_add(1, std::memory_order_relaxed);
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

                ssize_t w = 0;
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_TAPW, result->payload.size());
                  if (result->flags & FL_AGGR) {
                      // Codec lives in framing/Aggregate.hpp and is
                      // covered by tests; here we only care how many
                      // records the frame held and how many of those the
                      // TAP actually accepted. A short/failed write is
                      // silent otherwise -- the frame still counts as
                      // good because the first record landed.
                      size_t ok = 0, failed = 0;
                      size_t recs = aggregate::split(
                          result->payload.data(), result->payload.size(),
                          [&](const uint8_t* d, size_t l) {
                              ssize_t one = tap_->write(d, l);
                              if (one > 0) { w += one; ++ok; }
                              else         { ++failed; }
                          });
                      rx_records_.fetch_add(recs, std::memory_order_relaxed);
                      rx_rec_written_.fetch_add(ok, std::memory_order_relaxed);
                      rx_rec_failed_.fetch_add(failed, std::memory_order_relaxed);
                  } else {
                      w = tap_->write(result->payload.data(), result->payload.size());
                  } }
                if (w > 0) {
                    stats_.frames_rx_good.fetch_add(1, std::memory_order_relaxed);
                    stats_.bytes_rx.fetch_add(static_cast<uint64_t>(w),
                                              std::memory_order_relaxed);
                    // Frames Reed-Solomon actually REPAIRED, not merely
                    // frames that carried the FEC flag -- this counted every
                    // FEC-enabled frame, so it read as a 100% correction rate
                    // whenever FEC was on and told you nothing about whether
                    // RS was earning its parity overhead.
                    if (deframer.fecRescued() > 0)
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

        return this_frame_ok;
    };

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

        // One continuous pass over the WHOLE buffer, not per burst. Restarts
        // once per capture buffer rather than once per burst -- still far
        // fewer re-acquisitions, though true fabric would never restart at
        // all. That makes this measurement slightly pessimistic.
        if (tsync_freerun) {
            StageProfiler::Scope sc(prof_, StageProfiler::RX_TSYNC, iq_f.size());
            fres = fts.process(iq_f);
        }
        stats_.bursts_detected.fetch_add(windows.size(), std::memory_order_relaxed);
        {
            uint64_t occ = 0;
            for (const auto& w : windows) occ += (w.end > w.start) ? (w.end - w.start) : 0;
            stats_.rx_burst_samples.fetch_add(occ, std::memory_order_relaxed);
            stats_.rx_seen_samples .fetch_add(iq_f.size(), std::memory_order_relaxed);
        }

        for (const auto& win : windows) {
            // The detector marks where energy is, which is not the same as
            // where the frame ends: pulse shaping and threshold hysteresis
            // routinely clip a window well short of a full frame, and
            // demodulating a fragment can never satisfy the CRC. Extend to
            // at least one maximum-size frame (plus slack for the alignment
            // search) so a located frame is always complete.
            const size_t max_frame_samples =
                (MAX_PAYLOAD + WIRE_FRAME_OVERHEAD) * 8 *
                static_cast<size_t>(cfg_.samples_per_symbol);
            size_t win_end = std::min(iq_f.size(),
                                      std::max(win.end, win.start + max_frame_samples
                                                        + static_cast<size_t>(cfg_.burst_margin)));
            window_buf.assign(iq_f.begin() + static_cast<long>(win.start),
                              iq_f.begin() + static_cast<long>(win_end));

            if (tsync_freerun) {
                // res.idx[] holds the input sample index each symbol came
                // from, so the sample-domain burst window maps onto the
                // symbol stream exactly -- no second burst detector, and no
                // assumption that the decimation is exactly 4:1.
                auto lo = std::lower_bound(fres.idx.begin(), fres.idx.end(),
                                           static_cast<int>(win.start));
                auto hi = std::lower_bound(fres.idx.begin(), fres.idx.end(),
                                           static_cast<int>(win_end));
                if (lo >= hi) continue;
                std::vector<std::complex<float>> sym(
                    fres.syms.begin() + (lo - fres.idx.begin()),
                    fres.syms.begin() + (hi - fres.idx.begin()));

                { StageProfiler::Scope sc(prof_, StageProfiler::RX_CFO, sym.size());
                  CoarseFreqCorrect::apply(sym, symbol_rate, 0, sym.size()); }

                // No AGC runs in this path, so RSSI comes from the raw
                // window power instead of the AGC's gain word.
                double pw = 0.0;
                for (size_t k = win.start; k < win_end; ++k) pw += std::norm(iq_f[k]);
                pw = (win_end > win.start) ? pw / double(win_end - win.start) : 0.0;
                float rssi = (pw > 0) ? float(10.0 * std::log10(pw)) - 10.f : -120.f;
                float snr  = rssi + 95.f;
                stats_.rssi_dbm.store(rssi, std::memory_order_relaxed);
                stats_.snr_db  .store(snr,  std::memory_order_relaxed);

                bool decoded_here = false;
                size_t sfrom = 0;
                for (int fn = 0; fn < MAX_FRAMES_PER_WINDOW; ++fn) {
                    if (sfrom + HEADER_SYMS >= sym.size()) break;

                    // Locate the preamble BEFORE estimating carrier.
                    //
                    // The burst detector keeps burst_margin (512 samples =
                    // 128 symbols) of context ahead of the burst, so a slice
                    // that starts at the window edge begins in noise.
                    // DataAidedSync then computes its least-squares estimate
                    // over DAS_SEARCH_SYMS = 256 symbols that are half noise,
                    // half preamble, and derotates the whole burst by a wrong
                    // phase and frequency -- after which the sync correlation
                    // cannot match. Measured: 74% of detected bursts yielded
                    // no sync word at all, and goodput sat 5x below the
                    // windowed path.
                    //
                    // The windowed path never hits this because PreambleSync
                    // makes its slice start exactly at the preamble. Do the
                    // same one domain down.
                    std::vector<std::complex<float>> stail(
                        sym.begin() + static_cast<long>(sfrom), sym.end());
                    PreambleSync::Match smatch = psym.find(
                        stail, preamble_search /
                               static_cast<size_t>(cfg_.samples_per_symbol));
                    if (!smatch.found) break;
                    const size_t fstart = sfrom + static_cast<size_t>(smatch.offset);
                    if (fstart + HEADER_SYMS >= sym.size()) break;
                    iq_syms.assign(sym.begin() + static_cast<long>(fstart), sym.end());

                    // Same carrier recovery as the windowed path. The loop is
                    // already symbol-rate here, so nothing changes but the
                    // absence of a preceding AGC.
                    bool track_payload = false;
                    { StageProfiler::Scope sc(prof_, StageProfiler::RX_CARRIER, iq_syms.size());
                      switch (carrier_mode_) {
                        case CarrierMode::COSTAS: {
                            std::vector<std::complex<float>> o;
                            costas.reset(); costas.process(iq_syms, o); iq_syms.swap(o);
                            break; }
                        case CarrierMode::NONE: break;
                        case CarrierMode::LS: {
                            auto est = dasync.estimate(iq_syms, DAS_SEARCH_SYMS);
                            if (est.ok) DataAidedSync::derotate(iq_syms, est);
                            break; }
                        case CarrierMode::LS_COSTAS: {
                            auto est = dasync.estimate(iq_syms, DAS_SEARCH_SYMS);
                            if (est.ok) DataAidedSync::derotate(iq_syms, est);
                            track_payload = true;
                            break; }
                      } }

                    SplitModem::PayloadTap tap;
                    if (track_payload) {
                        tap = [&](std::vector<std::complex<float>>& pp) {
                            costas.reset();
                            std::vector<std::complex<float>> o;
                            costas.process(pp, o);
                            pp.swap(o);
                        };
                    }

                    SplitModem::Result sm;
                    { StageProfiler::Scope sc(prof_, StageProfiler::RX_DEMOD, iq_syms.size());
                      sm = SplitModem::demodulate(iq_syms, fec_ != nullptr,
                                                  SYNC_SEARCH_SYMS, tap); }
                    if (sm.header_ok) {
                        stats_.cur_mod.store(static_cast<int>(sm.payload_mod),
                                             std::memory_order_relaxed);
                        if (frame_log_.is_open())
                            frame_log_ << "[RX-HDR] mod=" << modCodeName(sm.payload_mod)
                                       << " plen=" << sm.plen
                                       << " sync_sym=" << sm.sync_sym
                                       << (sm.complete ? " complete" : " TRUNCATED") << "\n";
                    }
                    if (sm.complete) {
                        ok_any = false;
                        if (deliver(sm, rssi, snr, 0)) decoded_here = true;
                    }
                    // Advance even when the frame did not complete: a
                    // truncated frame at the tail of a window is routine, and
                    // abandoning the window on it throws away every later
                    // frame in a back-to-back burst. end_sym is relative to
                    // iq_syms, which starts at fstart.
                    size_t adv = (sm.complete && sm.end_sym > 0)
                                     ? sm.end_sym : psym.referenceLength();
                    sfrom = fstart + std::max<size_t>(adv, 1);
                }
                if (decoded_here)
                    stats_.bursts_demodulated.fetch_add(1, std::memory_order_relaxed);
                continue;   // free-running path done for this window
            }

            // Blind coarse CFO correction: two independent free-running
            // TCXOs (no shared reference clock) can produce an offset well
            // outside CostasLoop's native (and non-monotonic past that
            // range) pull-in range. Collapse it to a small residual before
            // AGC/Costas. Cheaper and more accurate now that it runs on an
            // isolated window instead of the whole batch.
            // Estimate from the region the detector actually flagged as
            // energy, but correct the whole extended window. The extension
            // exists so a clipped detection cannot truncate a real frame --
            // those extra samples are almost all noise and only dilute the
            // estimate, while costing search time proportional to their
            // number.
            const size_t est_lo = 0;
            const size_t est_hi = std::min(window_buf.size(),
                                           (win.end > win.start)
                                               ? (win.end - win.start)
                                               : window_buf.size());
            { StageProfiler::Scope sc(prof_, StageProfiler::RX_CFO, est_hi - est_lo);
              CoarseFreqCorrect::apply(window_buf, sample_rate, est_lo, est_hi); }

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
            size_t last_end_sym = 0, last_offset = 0;
            size_t frames_this_window = 0;
            int    frame_no = 0;
            for (; frame_no < MAX_FRAMES_PER_WINDOW; ++frame_no) {
                if (search_from >= window_buf.size()) {
                    walk_exit_eow_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                walk_iters_.fetch_add(1, std::memory_order_relaxed);

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
                if (!match.found) {
                    walk_exit_nosync_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                walk_sync_found_.fetch_add(1, std::memory_order_relaxed);

                const size_t base = search_from + static_cast<size_t>(match.offset);
                const size_t offsets[] = {
                    base,
                    // Neighbours, in case the peak sat a sample either side of
                    // the alignment TimingSync actually wants.
                    base > 0 ? base - 1 : 0,
                    base + 1,
                };

                bool this_frame_ok = false;
                for (size_t oi = 0; oi < ALIGN_OFFSETS && !this_frame_ok; ++oi) {
                size_t offset = offsets[oi];
                if (offset >= window_buf.size()) break;
                align_attempts_.fetch_add(1, std::memory_order_relaxed);
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
                  if (tsync_dropin) {
                      // Constructed fresh per attempt for the same reason
                      // tsync.reset() is called above: no state may carry
                      // over from a failed alignment. FixedTimingSync holds
                      // all loop state inside process(), so a fresh call is
                      // already a cold start.
                      auto fr = fts.process(offset_buf);
                      iq_timed = fr.syms;
                      if (slip_tracing) last_fts_ = std::move(fr);
                  } else {
                      tsync.process(offset_buf, iq_timed); // matched filter + symbol timing
                  } }

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
                bool track_payload = false;
                float cur_cfo = 0.f;
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
                        // Open-loop LS derotation over the whole burst. This
                        // uses only symbols the receiver already knows (the
                        // BPSK preamble and sync word), recovers *absolute*
                        // phase -- which resolves the constellation ambiguity
                        // outright -- and leaves a small residual drift.
                        //
                        // The loop that cleans up that residual is NOT run
                        // here; it runs on the payload alone, via the tap
                        // below. See SplitModem::PayloadTap.
                        iq_syms = iq_timed;
                        auto est = dasync.estimate(iq_timed, DAS_SEARCH_SYMS);
                        if (est.ok) {
                            DataAidedSync::derotate(iq_syms, est);
                            cur_cfo = est.freq_per_sym;
                            q_n_.fetch_add(1, std::memory_order_relaxed);
                            q_freq_abs_ur_.fetch_add(
                                static_cast<uint64_t>(std::fabs(est.freq_per_sym) * 1e6),
                                std::memory_order_relaxed);
                            q_qual_milli_.fetch_add(
                                static_cast<uint64_t>(est.quality * 1000.f),
                                std::memory_order_relaxed);
                        }
                        track_payload = true;
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
                // Residual carrier tracking, applied to the payload symbols
                // only. After the LS derotation above the residual starts at
                // roughly zero phase and frequency, so the loop begins from
                // rest and only has to follow the drift that open-loop
                // extrapolation accumulates towards the end of a frame.
                EvmStats cur_tail{};
                double   cur_evm_pay_post = 0.0;

                // Payload quality must be measured HERE, not on iq_syms.
                //
                // demodulate() applies this tap to an internal copy of the
                // payload symbols, so iq_syms still holds them BEFORE residual
                // carrier tracking -- still rotating. Measuring there reported
                // ~68% payload EVM with p95 over 100% on frames that passed
                // CRC cleanly, because it was scoring symbols the demodulator
                // never decides on. After the swap below, `p` holds exactly
                // what the bit decisions are taken from.
                double cur_evm_acq2 = 0.0;
                SplitModem::PayloadTap tap;
                if (track_payload) {
                    // Costas seeding (SDR_COSTAS_SEED):
                    //   none  reset to phase 0, freq 0 -- current behaviour
                    //   freq  seed the NCO frequency with the LS estimate
                    //   resid seed with the estimate's EXTRAPOLATION ERROR
                    //
                    // Note derotate() has already removed est.freq_per_sym
                    // from these symbols, so "freq" re-applies a ramp that is
                    // no longer present -- it is measured here rather than
                    // assumed. "resid" is the variant that matches the intent
                    // of turning Costas into a tracker: it seeds what the
                    // loop actually has left to follow, which is the error in
                    // the LS fit extrapolated across the payload, not the fit
                    // itself.
                    const float seed_freq =
                        (costas_seed_ == 1) ? cur_cfo :
                        (costas_seed_ == 2) ? (cur_cfo * 0.05f) : 0.f;
                    tap = [&, seed_freq](std::vector<std::complex<float>>& p) {
                        // Weak symbols cluster in the first quarter of the
                        // payload (98% of them) at BOTH bandwidths. Measure
                        // the payload either side of this loop to find out
                        // whether Costas causes that transient or inherits
                        // it: amplitude tells an AGC/filter settle apart from
                        // a phase-tracking transient, and comparing in
                        // against out says which side of the loop it is on.
                        auto probe = [](const std::vector<std::complex<float>>& v,
                                        double& amp_q1, double& amp_rest,
                                        double& ph_q1, double& ph_rest) {
                            if (v.size() < 8) return;
                            const size_t q1 = v.size() / 4;
                            double a1=0, a2=0, p1=0, p2=0;
                            for (size_t i = 0; i < v.size(); ++i) {
                                const double m = std::abs(v[i]);
                                // Phase error to the nearest QPSK decision,
                                // folded into +-45 degrees.
                                double ph = std::arg(v[i]);
                                double f = std::fmod(ph + M_PI/4.0, M_PI/2.0);
                                if (f < 0) f += M_PI/2.0;
                                const double e = std::fabs(f - M_PI/4.0);
                                if (i < q1) { a1 += m; p1 += e; }
                                else        { a2 += m; p2 += e; }
                            }
                            amp_q1   = a1 / double(q1);
                            amp_rest = a2 / double(v.size() - q1);
                            ph_q1    = p1 / double(q1);
                            ph_rest  = p2 / double(v.size() - q1);
                        };
                        double ia1=0, ia2=0, ip1=0, ip2=0;
                        probe(p, ia1, ia2, ip1, ip2);

                        std::vector<std::complex<float>> pre;
                        if (slip_trace_.is_open()) pre = p;

                        if (costas_seed_ != 0) costas.seed(0.f, seed_freq);
                        else                    costas.reset();
                        costas.setPhaseLimit(costas_phase_limit_);
                        std::vector<std::complex<float>> out;
                        costas.process(p, out);
                        p.swap(out);

                        // Rotation the loop actually applied, per symbol.
                        if (!pre.empty() && pre.size() == p.size()) {
                            last_carrier_phase_.resize(p.size());
                            for (size_t i = 0; i < p.size(); ++i)
                                last_carrier_phase_[i] =
                                    std::arg(p[i] * std::conj(pre[i]));
                        }

                        double oa1=0, oa2=0, op1=0, op2=0;
                        probe(p, oa1, oa2, op1, op2);
                        if (ia2 > 0 && oa2 > 0) {
                            cs_in_amp_ratio_ .fetch_add(uint64_t(1000.0 * ia1 / ia2), std::memory_order_relaxed);
                            cs_out_amp_ratio_.fetch_add(uint64_t(1000.0 * oa1 / oa2), std::memory_order_relaxed);
                            cs_in_ph_q1_  .fetch_add(uint64_t(1000.0 * ip1), std::memory_order_relaxed);
                            cs_in_ph_rest_.fetch_add(uint64_t(1000.0 * ip2), std::memory_order_relaxed);
                            cs_out_ph_q1_ .fetch_add(uint64_t(1000.0 * op1), std::memory_order_relaxed);
                            cs_out_ph_rest_.fetch_add(uint64_t(1000.0 * op2), std::memory_order_relaxed);
                            cs_probe_n_.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (!p.empty()) {
                            const bool q = (tx_mod_ != ModCode::BPSK);
                            cur_tail = evmStats(p, 0, p.size(), q);
                            cur_evm_pay_post = cur_tail.rms;
                        }
                    };
                }
                (void)cur_evm_acq2;

                double cur_evm_acq = 0.0, cur_evm_pay = 0.0;
                bool   cur_evm_ok = false;
                SplitModem::Result sm;
                { StageProfiler::Scope sc(prof_, StageProfiler::RX_DEMOD, iq_syms.size());
                  sm = SplitModem::demodulate(iq_syms, fec_ != nullptr,
                                              SYNC_SEARCH_SYMS, tap); }
                if (sm.header_ok) {
                    // Acquisition is always BPSK; payload is whatever the
                    // header declared. Splitting the two separates a carrier
                    // fault (both degrade) from a payload-only fault.
                    size_t a0 = sm.sync_sym;
                    size_t a1 = std::min(a0 + HEADER_SYMS, iq_syms.size());
                    size_t p1 = std::min(sm.end_sym, iq_syms.size());
                    if (a1 > a0 && p1 > a1) {
                        cur_evm_acq = evmPct(iq_syms, a0, a1, false);
                        // Payload figure comes from the tap (post-tracking);
                        // falls back to 0 when no tap ran (carrier_mode NONE).
                        cur_evm_pay = cur_evm_pay_post;
                        cur_evm_ok  = true;
                        q_evm_acq_pct_.fetch_add(static_cast<uint64_t>(cur_evm_acq),
                                                 std::memory_order_relaxed);
                        q_evm_pay_pct_.fetch_add(static_cast<uint64_t>(cur_evm_pay),
                                                 std::memory_order_relaxed);
                        q_evm_n_.fetch_add(1, std::memory_order_relaxed);
                    }
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

                ok_any = false;
                this_frame_ok = deliver(sm, rssi, snr, oi);

                // Carrier-phase trace, dumped for BOTH outcomes.
                //
                // The control group is the point: if frames that PASS CRC
                // show the same +-pi/2 jumps as frames that fail, the
                // detector is measuring noise and proves nothing. A previous
                // "smoking gun" here turned out to be an artifact of the
                // diagnostic tool, so the tool now carries its own control.
                if (slip_trace_.is_open() && sm.header_ok
                    && !last_carrier_phase_.empty()) {
                    // Size-match the control group. Failures are ~4192-symbol
                    // frames and passes ~900, so an unmatched comparison of
                    // any "maximum over the frame" statistic is confounded by
                    // sample count alone.
                    const bool size_ok = (last_carrier_phase_.size() > 3000);
                    int& cap = this_frame_ok ? slip_pass_dumped_ : slip_frames_dumped_;
                    if (this_frame_ok && !size_ok) { /* skip short controls */ } else
                    if (cap < 10) {
                        const uint8_t* bb = sm.bytes.data();
                        uint32_t sq = sm.bytes.size() >= 16
                            ? ((uint32_t(bb[12])<<24)|(uint32_t(bb[13])<<16)
                              |(uint32_t(bb[14])<< 8)| uint32_t(bb[15])) : 0;
                        const size_t n = last_carrier_phase_.size();
                        // Largest single-symbol phase step, and how many steps
                        // land near +-pi/2 (a QPSK ambiguity slip).
                        float mx = 0.f; size_t mx_at = 0; int near90 = 0;
                        for (size_t i = 1; i < n; ++i) {
                            float d = last_carrier_phase_[i] - last_carrier_phase_[i-1];
                            while (d >  3.14159265f) d -= 6.28318531f;
                            while (d < -3.14159265f) d += 6.28318531f;
                            const float ad = std::fabs(d);
                            if (ad > mx) { mx = ad; mx_at = i; }
                            if (ad > 1.2f && ad < 1.95f) ++near90;
                        }
                        // Per-symbol steps ruled out a cycle slip (max 0.57
                        // rad, never near pi/2). The remaining carrier
                        // mechanism is smooth DRIFT: phase creeping past 45
                        // degrees flips every subsequent QPSK symbol without
                        // ever producing a large single-symbol delta.
                        //
                        // Measure the accumulated rotation relative to the
                        // frame start, unwrapped, and how far it strays from
                        // the straight line through it -- a residual
                        // frequency error shows as a ramp, a wander as
                        // curvature. 0.785 rad (45 deg) is the decision
                        // boundary where QPSK bits start flipping.
                        double cum = 0.0, cum_max = 0.0, cum_end = 0.0;
                        size_t cum_at = 0; int past45 = 0;
                        {
                            double acc = 0.0;
                            for (size_t i = 1; i < n; ++i) {
                                float d = last_carrier_phase_[i] - last_carrier_phase_[i-1];
                                while (d >  3.14159265f) d -= 6.28318531f;
                                while (d < -3.14159265f) d += 6.28318531f;
                                acc += d;
                                if (std::fabs(acc) > cum_max) { cum_max = std::fabs(acc); cum_at = i; }
                                if (std::fabs(acc) > 0.785398) ++past45;
                            }
                            cum = acc; cum_end = acc;
                        }
                        (void)cum;
                        slip_trace_ << (this_frame_ok ? "PASS" : "FAIL")
                                    << " seq=" << sq
                                    << " nsym=" << n
                                    << " max_dphi=" << mx
                                    << " at_sym=" << mx_at
                                    << " frac=" << (n ? double(mx_at)/double(n) : 0.0)
                                    << " near90_steps=" << near90
                                    << " cum_end=" << cum_end
                                    << " cum_max=" << cum_max
                                    << " cum_at=" << (n ? double(cum_at)/double(n) : 0.0)
                                    << " syms_past45=" << past45 << "\n";
                        ++cap;
                    }
                }
                {
                    // Structural attribution, independent of EVM.
                    const size_t need = HEADER_SIZE + sm.plen + 4;
                    auto& plen  = this_frame_ok ? st_pass_plen_  : st_fail_plen_;
                    auto& inv   = this_frame_ok ? st_pass_inv_   : st_fail_inv_;
                    auto& shrt  = this_frame_ok ? st_pass_short_ : st_fail_short_;
                    auto& aggr  = this_frame_ok ? st_pass_aggr_  : st_fail_aggr_;
                    plen.fetch_add(sm.plen, std::memory_order_relaxed);
                    if (cur_tail.n) {
                        auto& p95  = this_frame_ok ? t_pass_p95_  : t_fail_p95_;
                        auto& p99  = this_frame_ok ? t_pass_p99_  : t_fail_p99_;
                        auto& mx   = this_frame_ok ? t_pass_max_  : t_fail_max_;
                        auto& wk   = this_frame_ok ? t_pass_weak_ : t_fail_weak_;
                        auto& tn   = this_frame_ok ? t_pass_n_    : t_fail_n_;
                        p95.fetch_add(static_cast<uint64_t>(cur_tail.p95), std::memory_order_relaxed);
                        p99.fetch_add(static_cast<uint64_t>(cur_tail.p99), std::memory_order_relaxed);
                        mx .fetch_add(static_cast<uint64_t>(cur_tail.max), std::memory_order_relaxed);
                        wk .fetch_add(cur_tail.weak, std::memory_order_relaxed);
                        tn .fetch_add(1, std::memory_order_relaxed);
                        auto& wp = this_frame_ok ? t_pass_wpos_ : t_fail_wpos_;
                        auto& wf = this_frame_ok ? t_pass_wfq_  : t_fail_wfq_;
                        auto& wl = this_frame_ok ? t_pass_wlq_  : t_fail_wlq_;
                        wp.fetch_add(static_cast<uint64_t>(cur_tail.worst_pos * 1000.0),
                                     std::memory_order_relaxed);
                        wf.fetch_add(cur_tail.weak_first_q, std::memory_order_relaxed);
                        wl.fetch_add(cur_tail.weak_last_q,  std::memory_order_relaxed);
                    }
                    if (sm.inverted)                 inv .fetch_add(1, std::memory_order_relaxed);
                    if (sm.bytes.size() < need)      shrt.fetch_add(1, std::memory_order_relaxed);
                    if (sm.flags & FL_AGGR)          aggr.fetch_add(1, std::memory_order_relaxed);
                    if (!this_frame_ok && sm.payload_mod != tx_mod_)
                        st_fail_modbad_.fetch_add(1, std::memory_order_relaxed);
                }
                if (cur_evm_ok) {
                    auto& n   = this_frame_ok ? q_pass_n_   : q_fail_n_;
                    auto& acq = this_frame_ok ? q_pass_acq_ : q_fail_acq_;
                    auto& pay = this_frame_ok ? q_pass_pay_ : q_fail_pay_;
                    auto& cfo = this_frame_ok ? q_pass_cfo_ur_ : q_fail_cfo_ur_;
                    n  .fetch_add(1, std::memory_order_relaxed);
                    acq.fetch_add(static_cast<uint64_t>(cur_evm_acq), std::memory_order_relaxed);
                    pay.fetch_add(static_cast<uint64_t>(cur_evm_pay), std::memory_order_relaxed);
                    cfo.fetch_add(static_cast<uint64_t>(std::fabs(cur_cfo) * 1e6),
                                  std::memory_order_relaxed);
                }
                if (ok_any) decoded_any = true;
                if (this_frame_ok) {
                    // Remember where this frame ENDED so the walk can skip it.
                    last_end_sym = sm.end_sym;
                    last_offset  = offset;
                }
                }   // end alignment-neighbour loop

                // Advance past the frame that was just decoded.
                //
                // This used to step forward by psync.referenceLength() -- the
                // length of the preamble+sync correlation template, 704
                // samples. A full frame is ~18300 samples, so the walk moved
                // 1/26th of a frame per iteration and spent the next 25
                // iterations re-correlating inside data it had already
                // consumed. With MAX_FRAMES_PER_WINDOW = 64 it could not
                // traverse more than ~2.4 frames of a window at all.
                //
                // Node A aggregates ~2 frames per burst and node B recovered
                // ~0.83 of them; the decoded frame rate sat at ~55/s whether
                // the channel was 1 MHz or 2 MHz, because the limit was this
                // loop rather than the air.
                //
                // end_sym is the symbol index one past the decoded frame,
                // relative to the symbol stream that began at `offset`, so
                // offset + end_sym*sps is where the next frame can start.
                if (this_frame_ok && last_end_sym > 0) {
                    const size_t adv = last_offset +
                        static_cast<size_t>(last_end_sym) * static_cast<size_t>(RRC_SPS);
                    // Never go backwards, and always make progress.
                    search_from = (adv > search_from) ? adv
                                : search_from + psync.referenceLength();
                    walk_adv_frame_.fetch_add(1, std::memory_order_relaxed);
                    ++frames_this_window;
                    walk_frames_ok_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // No decode here: step by the template so a stubborn
                    // correlation peak cannot spin us in place.
                    search_from = base + psync.referenceLength();
                    walk_adv_ref_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (frame_no >= MAX_FRAMES_PER_WINDOW)
                walk_exit_maxframes_.fetch_add(1, std::memory_order_relaxed);
            walk_frames_in_window_.fetch_add(frames_this_window, std::memory_order_relaxed);
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

        // Duty over this tick only. tx_air_seconds_ is cumulative, so the
        // delta divided by elapsed time is the real occupancy the limiter is
        // meant to cap -- and the one the peer's burst detector experiences.
        {
            double air = tx_air_seconds_.load(std::memory_order_relaxed);
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
