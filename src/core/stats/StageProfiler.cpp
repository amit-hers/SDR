#include "sdr/stats/StageProfiler.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unistd.h>

namespace sdr {

namespace {
// Which stages count samples, which count symbols, which count bytes -- so the
// report can print a meaningful rate rather than a bare number.
const char* unitFor(int s) {
    switch (s) {
        case StageProfiler::RX_PULL:
        case StageProfiler::RX_CONVERT:
        case StageProfiler::RX_FFT:
        case StageProfiler::RX_DETECT:
        case StageProfiler::RX_CFO:
        case StageProfiler::RX_PSYNC:
        case StageProfiler::RX_AGC:
        case StageProfiler::RX_TSYNC:
        case StageProfiler::TX_RRC:
        case StageProfiler::TX_CONV:
        case StageProfiler::TX_PUSH:  return "Msamp/s";
        case StageProfiler::RX_CARRIER:
        case StageProfiler::RX_DEMOD:
        case StageProfiler::TX_MOD:   return "Msym/s";
        default:                      return "kB/s";
    }
}
} // namespace

const char* StageProfiler::name(int s) {
    switch (s) {
        case RX_PULL:    return "RX rxPull (libiio)";
        case RX_CONVERT: return "RX int16->complex";
        case RX_FFT:     return "RX FFT spectrum";
        case RX_DETECT:  return "RX BurstDetector";
        case RX_CFO:     return "RX CoarseFreqCorrect";
        case RX_PSYNC:   return "RX PreambleSync";
        case RX_AGC:     return "RX AGC";
        case RX_TSYNC:   return "RX TimingSync";
        case RX_CARRIER: return "RX carrier (Costas/LS)";
        case RX_DEMOD:   return "RX SplitModem demod";
        case RX_DEFRAME: return "RX Deframer+CRC/FEC";
        case RX_TAPW:    return "RX TAP write";
        case TX_TAPR:    return "TX TAP read";
        case TX_FRAME:   return "TX Framer+CRC/FEC";
        case TX_MOD:     return "TX SplitModem mod";
        case TX_RRC:     return "TX RRC interp";
        case TX_CONV:    return "TX float->int16";
        case TX_PUSH:    return "TX txPush (libiio)";
        default:         return "?";
    }
}

void StageProfiler::reset() {
    for (int i = 0; i < N_STAGES; ++i) {
        ns_[static_cast<size_t>(i)].store(0, std::memory_order_relaxed);
        calls_[static_cast<size_t>(i)].store(0, std::memory_order_relaxed);
        items_[static_cast<size_t>(i)].store(0, std::memory_order_relaxed);
    }
}

double StageProfiler::processCpuSeconds() {
    std::ifstream f("/proc/self/stat");
    if (!f) return 0.0;
    std::string tok;
    // utime is field 14, stime field 15; the comm field can contain spaces so
    // skip to after the closing ')' first.
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto rp = all.rfind(')');
    if (rp == std::string::npos) return 0.0;
    std::istringstream ss(all.substr(rp + 1));
    long long v = 0, utime = 0, stime = 0;
    // field 3 (state) onward: we need fields 14 and 15 overall, i.e. the 11th
    // and 12th tokens after the ')'.
    for (int i = 0; i < 12; ++i) {
        if (!(ss >> tok)) return 0.0;
        try { v = std::stoll(tok); } catch (...) { v = 0; }
        if (i == 10) utime = v;
        if (i == 11) stime = v;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return static_cast<double>(utime + stime) / static_cast<double>(hz);
}

double StageProfiler::residentMb() {
    std::ifstream f("/proc/self/statm");
    if (!f) return 0.0;
    long long total = 0, rss = 0;
    f >> total >> rss;
    return static_cast<double>(rss) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

std::string StageProfiler::report(double wall_seconds) const {
    std::ostringstream o;
    o << std::fixed;
    o << "\n=== stage profile (" << std::setprecision(1) << wall_seconds << " s wall) ===\n";
    o << std::left << std::setw(24) << "stage"
      << std::right << std::setw(10) << "cpu_s"
      << std::setw(9)  << "%core"
      << std::setw(12) << "calls"
      << std::setw(11) << "us/call"
      << std::setw(13) << "rate"
      << "  unit\n";

    double total_ns = 0;
    for (int i = 0; i < N_STAGES; ++i)
        total_ns += static_cast<double>(ns_[static_cast<size_t>(i)].load(std::memory_order_relaxed));

    for (int i = 0; i < N_STAGES; ++i) {
        double ns    = static_cast<double>(ns_[static_cast<size_t>(i)].load(std::memory_order_relaxed));
        double calls = static_cast<double>(calls_[static_cast<size_t>(i)].load(std::memory_order_relaxed));
        double items = static_cast<double>(items_[static_cast<size_t>(i)].load(std::memory_order_relaxed));
        if (calls == 0) continue;
        double sec = ns / 1e9;
        double rate = 0;
        const char* unit = unitFor(i);
        if (sec > 0 && items > 0) {
            rate = items / sec;
            if (std::string(unit) == "kB/s") rate /= 1e3;
            else                             rate /= 1e6;
        }
        o << std::left << std::setw(24) << name(i) << std::right
          << std::setw(10) << std::setprecision(3) << sec
          << std::setw(8)  << std::setprecision(1) << (wall_seconds > 0 ? 100.0 * sec / wall_seconds : 0.0) << "%"
          << std::setw(12) << std::setprecision(0) << calls
          << std::setw(11) << std::setprecision(1) << (calls > 0 ? ns / calls / 1e3 : 0.0)
          << std::setw(13) << std::setprecision(2) << rate
          << "  " << unit << "\n";
    }
    o << std::left << std::setw(24) << "TOTAL measured" << std::right
      << std::setw(10) << std::setprecision(3) << total_ns / 1e9
      << std::setw(8)  << std::setprecision(1)
      << (wall_seconds > 0 ? 100.0 * (total_ns / 1e9) / wall_seconds : 0.0) << "%\n";
    return o.str();
}

} // namespace sdr
