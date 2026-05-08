#include "ScanMode.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <complex>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>

namespace sdr {

ScanMode::ScanMode(const Config& cfg, PlutoSDR& radio)
    : cfg_(cfg), radio_(radio) {}

ScanMode::~ScanMode() { stop(); }

void ScanMode::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&ScanMode::scanThread, this);
}

void ScanMode::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

float ScanMode::measurePower(double freq_hz) {
    radio_.setRxFrequency(freq_hz);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<int16_t> iq(MEASURE_SAMPLES * 2);
    int n = radio_.rxPull(iq.data(), MEASURE_SAMPLES);
    if (n <= 0) return -120.0f;

    double pwr = 0.0;
    for (int i = 0; i < n; ++i) {
        float I = iq[static_cast<size_t>(i)*2]   / 2048.0f;
        float Q = iq[static_cast<size_t>(i)*2+1] / 2048.0f;
        pwr += static_cast<double>(I*I + Q*Q);
    }
    pwr /= static_cast<double>(n);
    return (pwr > 0.0) ? 10.0f * std::log10(static_cast<float>(pwr)) : -120.0f;
}

void ScanMode::scanThread() {
    results_.clear();

    for (int i = 0; i < cfg_.scan_n && running_.load(); ++i) {
        double freq = cfg_.scan_start_mhz + i * cfg_.scan_step_mhz;
        float  pwr  = measurePower(freq * 1e6);
        results_.push_back({freq, pwr});
    }

    // Write scan.json
    std::ostringstream j;
    j << "[";
    for (size_t i = 0; i < results_.size(); ++i) {
        j << "{\"freq_mhz\":" << std::fixed << std::setprecision(3)
          << results_[i].freq_mhz
          << ",\"power_dbm\":" << std::setprecision(1)
          << results_[i].power_dbm << "}";
        if (i + 1 < results_.size()) j << ",";
    }
    j << "]";

    std::ofstream f("/tmp/sdr_scan.json", std::ios::trunc);
    if (f) f << j.str();

    running_.store(false);
}

} // namespace sdr
