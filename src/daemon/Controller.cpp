#include "Controller.hpp"
#include "modes/BridgeMode.hpp"
#include "modes/MeshMode.hpp"
#include "modes/P2PMode.hpp"
#include "modes/ScanMode.hpp"
#include <csignal>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <chrono>

namespace sdr {

Controller* Controller::instance_ = nullptr;

static void sigHandler(int sig) {
    if (sig == SIGUSR2 && Controller::instance())
        Controller::instance()->requestReload();
    // SIGINT/SIGTERM handled by run() loop via flag
}

Controller::Controller(Config cfg) : cfg_(std::move(cfg)) {
    instance_ = this;

    radio_ = PlutoSDR::connect(cfg_.pluto_ip);
    radio_->setTxFrequency(cfg_.freq_tx_mhz * 1e6);
    radio_->setRxFrequency(cfg_.freq_rx_mhz * 1e6);
    radio_->setTxAttenuation(cfg_.tx_atten_db);
    radio_->setBandwidth(static_cast<long long>(cfg_.bw_mhz) * 1'000'000LL);
    radio_->setSampleRate(static_cast<long long>(cfg_.bw_mhz) * 1'000'000LL * 4);
    radio_->setGainMode(cfg_.gain_mode);

    mode_     = makeMode();
    exporter_ = std::make_unique<StatsExporter>(mode_->stats(), cfg_.stats_interval_ms);
}

std::unique_ptr<IMode> Controller::makeMode() {
    if (cfg_.mode == "bridge")
        return std::make_unique<BridgeMode>(cfg_, *radio_);
    if (cfg_.mode == "mesh")
        return std::make_unique<MeshMode>(cfg_, *radio_);
    if (cfg_.mode == "p2p-tx" || cfg_.mode == "p2p-rx")
        return std::make_unique<P2PMode>(cfg_, *radio_);
    if (cfg_.mode == "scan")
        return std::make_unique<ScanMode>(cfg_, *radio_);
    throw std::invalid_argument("Unknown mode: " + cfg_.mode);
}

void Controller::setupSignals() {
    struct sigaction sa{};
    sa.sa_handler = sigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, nullptr);

    // Graceful shutdown on SIGINT/SIGTERM handled by blocking on flag
    signal(SIGINT,  [](int){ Controller::instance()->mode_->stop(); });
    signal(SIGTERM, [](int){ Controller::instance()->mode_->stop(); });
}

void Controller::run() {
    setupSignals();
    std::cout << "[sdr] Starting mode=" << cfg_.mode
              << " freq_tx=" << cfg_.freq_tx_mhz
              << " freq_rx=" << cfg_.freq_rx_mhz
              << " bw=" << cfg_.bw_mhz << "MHz\n";

    mode_->start();
    exporter_->start();

    while (mode_->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (reload_requested_.exchange(false)) {
            std::cout << "[sdr] SIGUSR2: reloading config\n";
            try {
                Config fresh = Config::fromFile("/tmp/sdr_reload.json");
                applyLive(fresh.tx_atten_db,
                          fresh.freq_tx_mhz * 1e6,
                          fresh.freq_rx_mhz * 1e6, 0);
            } catch (const std::exception& e) {
                std::cerr << "[sdr] reload error: " << e.what() << "\n";
            }
        }
    }

    exporter_->stop();
    std::cout << "[sdr] Stopped.\n";
}

void Controller::applyLive(int atten_db, double freq_tx_hz,
                            double freq_rx_hz, int /*mod_code*/) {
    if (atten_db >= 0)    radio_->setTxAttenuation(atten_db);
    if (freq_tx_hz > 0.0) radio_->setTxFrequency(freq_tx_hz);
    if (freq_rx_hz > 0.0) radio_->setRxFrequency(freq_rx_hz);
}

} // namespace sdr
