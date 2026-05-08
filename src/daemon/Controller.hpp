#pragma once
#include "Config.hpp"
#include "StatsExporter.hpp"
#include "modes/IMode.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include <memory>
#include <atomic>

namespace sdr {

class Controller {
public:
    explicit Controller(Config cfg);

    // Blocks until SIGINT / SIGTERM
    void run();

    void requestReload() { reload_requested_.store(true); }

    // Live-tune without restart (called from signal handler / monitor API)
    void applyLive(int atten_db, double freq_tx_hz, double freq_rx_hz, int mod_code);

    static Controller* instance() { return instance_; }

private:
    void setupSignals();
    std::unique_ptr<IMode> makeMode();

    Config                         cfg_;
    std::unique_ptr<PlutoSDR>      radio_;
    std::unique_ptr<IMode>         mode_;
    std::unique_ptr<StatsExporter> exporter_;

    std::atomic<bool> reload_requested_{false};

    static Controller* instance_;
};

} // namespace sdr
