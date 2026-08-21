#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace sdr {

// Per-stage wall-clock accounting for the TX and RX pipelines.
//
// The point is to answer "where does the CPU actually go" with measurements
// rather than intuition, before any architectural work. Optimising the wrong
// stage is worse than not optimising: it costs effort and can trade away RF
// reliability for nothing.
//
// Counters are plain relaxed atomics because two threads (TX and RX) write
// disjoint stages and nothing reads them until the report. Cost per sample is
// one clock_gettime pair plus two relaxed adds, which is negligible next to
// the DSP being measured -- but it is still opt-in via $SDR_PROFILE so the
// production path stays untouched.
class StageProfiler {
public:
    enum Stage : int {
        // RX
        RX_PULL = 0,   // rxPull: libiio refill + copy
        RX_CONVERT,    // int16 -> complex<float>
        RX_FFT,        // spectrum accumulation (display only)
        RX_DETECT,     // BurstDetector
        RX_CFO,        // CoarseFreqCorrect
        RX_PSYNC,      // PreambleSync correlation
        RX_AGC,        // AGC
        RX_TSYNC,      // TimingSync (matched filter + timing recovery)
        RX_CARRIER,    // Costas / LS derotation
        RX_DEMOD,      // SplitModem::demodulate
        RX_DEFRAME,    // Deframer + CRC + FEC
        RX_TAPW,       // TAP write
        // TX
        TX_TAPR,       // TAP read
        TX_FRAME,      // Framer (+CRC/FEC/AES)
        TX_MOD,        // SplitModem::modulate
        TX_RRC,        // RRC interpolation
        TX_CONV,       // float -> int16
        TX_PUSH,       // txPush
        N_STAGES
    };

    static const char* name(int s);

    void enable(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void add(int stage, uint64_t ns, uint64_t items = 0) {
        ns_[static_cast<size_t>(stage)].fetch_add(ns, std::memory_order_relaxed);
        calls_[static_cast<size_t>(stage)].fetch_add(1, std::memory_order_relaxed);
        if (items)
            items_[static_cast<size_t>(stage)].fetch_add(items, std::memory_order_relaxed);
    }

    void reset();

    // Human-readable table: per-stage time, share of one core, calls, and
    // throughput where the stage counts items (samples, symbols, bytes).
    std::string report(double wall_seconds) const;

    // Process CPU seconds (user+sys) from /proc/self/stat.
    static double processCpuSeconds();
    // Resident set size in MB.
    static double residentMb();

    // Scoped timer. Does nothing measurable when profiling is disabled.
    class Scope {
    public:
        Scope(StageProfiler& p, int stage, uint64_t items = 0)
            : p_(p), stage_(stage), items_(items), on_(p.enabled()) {
            if (on_) t0_ = std::chrono::steady_clock::now();
        }
        ~Scope() {
            if (!on_) return;
            auto dt = std::chrono::steady_clock::now() - t0_;
            p_.add(stage_, static_cast<uint64_t>(
                       std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()),
                   items_);
        }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        StageProfiler& p_;
        int            stage_;
        uint64_t       items_;
        bool           on_;
        std::chrono::steady_clock::time_point t0_;
    };

private:
    std::atomic<bool> enabled_{false};
    std::array<std::atomic<uint64_t>, N_STAGES> ns_{};
    std::array<std::atomic<uint64_t>, N_STAGES> calls_{};
    std::array<std::atomic<uint64_t>, N_STAGES> items_{};
};

} // namespace sdr
