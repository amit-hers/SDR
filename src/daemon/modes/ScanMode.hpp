#pragma once
#include "IMode.hpp"
#include "../Config.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/dsp/FFTSpectrum.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <string>

namespace sdr {

struct ScanResult {
    double freq_mhz;
    float  power_dbm;
};

class ScanMode : public IMode {
public:
    explicit ScanMode(const Config& cfg, PlutoSDR& radio);
    ~ScanMode() override;

    void  start()          override;
    void  stop()           override;
    bool  running() const  override { return running_.load(); }
    const LinkStats& stats() const override { return stats_; }

    const std::vector<ScanResult>& results() const { return results_; }

private:
    void scanThread();
    float measurePower(double freq_hz);

    const Config&     cfg_;
    PlutoSDR&         radio_;
    LinkStats         stats_;
    std::vector<ScanResult> results_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    static constexpr size_t MEASURE_SAMPLES = 8192;
};

} // namespace sdr
