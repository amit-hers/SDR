#include "Controller.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/CoarseFreqCorrect.hpp"
#include "modes/BridgeMode.hpp"
#include "modes/MeshMode.hpp"
#include "modes/P2PMode.hpp"
#include "modes/ScanMode.hpp"
#include <csignal>
#include <cstdlib>
#include <string>
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
    // The analog filter has to pass the *occupied* bandwidth, not the symbol
    // rate. Root-raised-cosine shaping at rolloff 0.35 spreads the signal
    // over 1.35x the symbol rate, so setting the filter to the symbol rate
    // (as this did) clips roughly a quarter of the spectrum -- the pulse
    // skirts, which is exactly the energy the matched filter needs to
    // reconstruct symbol shape. 1.4x leaves a little margin above 1.35.
    //
    // cfg_.bw_mhz is the symbol rate in MHz; samples_per_symbol controls the
    // host-side I/Q rate. Both ends must use the same value.
    const double symbol_rate = static_cast<double>(cfg_.bw_mhz) * 1e6;
    const double analog_bw   = symbol_rate * cfg_.rx_bw_factor;
    radio_->setBandwidth(static_cast<long long>(analog_bw));
    radio_->setSampleRate(static_cast<long long>(symbol_rate) *
                          cfg_.samples_per_symbol);
    std::cerr << "[sdr] symbol rate " << symbol_rate / 1e6 << " MHz -> analog "
                 "bandwidth " << analog_bw / 1e6 << " MHz (RRC rolloff "
              << RRC_ROLLOFF << " occupies " << (1.0 + RRC_ROLLOFF)
              << "x; factor=" << cfg_.rx_bw_factor << "); "
              << cfg_.samples_per_symbol << " samples/symbol\n";
    radio_->setGainMode(cfg_.gain_mode);

    CoarseFreqCorrect::setMethod(cfg_.cfo_method == "grid"
                                 ? CoarseFreqCorrect::Method::GRID
                                 : CoarseFreqCorrect::Method::FFT);
    std::cerr << "[sdr] coarse CFO estimator: " << cfg_.cfo_method << "\n";

    mode_     = makeMode();
    // State reporting for sdrctl. Built from the same LinkStats the JSON
    // exporter uses, so the two can never disagree about what the node is
    // doing. A failure to bind is logged and ignored -- telemetry is a
    // convenience and must not be able to stop the radio.
    if (cfg_.telemetry_port > 0) {
        const LinkStats& st = mode_->stats();
        telemetry_ = std::make_unique<TelemetryServer>(
            [this, &st](StatePacket& p) {
                p.node_id     = cfg_.node_id_u32;
                p.uptime_s    = static_cast<uint32_t>(st.uptime_s.load());
                p.mode        = cfg_.mode == "bridge" ? 1 : cfg_.mode == "mesh"   ? 2
                              : cfg_.mode == "p2p-tx" ? 3 : cfg_.mode == "p2p-rx" ? 4
                              : cfg_.mode == "scan"   ? 5 : 0;
                p.freq_tx_hz  = static_cast<uint32_t>(cfg_.freq_tx_mhz * 1e6);
                p.freq_rx_hz  = static_cast<uint32_t>(cfg_.freq_rx_mhz * 1e6);
                p.bw_hz       = static_cast<uint32_t>(cfg_.bw_mhz * 1e6);
                p.sps         = static_cast<uint8_t>(cfg_.samples_per_symbol);
                p.modulation  = static_cast<uint8_t>(st.cur_mod.load());
                p.tx_atten_cdb= static_cast<int16_t>(cfg_.tx_atten_db * 100.0);
                p.frames_tx      = st.frames_tx.load();
                p.frames_rx_good = st.frames_rx_good.load();
                p.frames_rx_bad  = st.frames_rx_bad.load();
                p.dropped        = st.dropped.load();
                p.bytes_tx       = st.bytes_tx.load();
                p.bytes_rx       = st.bytes_rx.load();
                p.fec_corrected  = st.fec_corrected.load();
                p.rssi_cdbm   = static_cast<int16_t>(st.rssi_dbm.load() * 100.f);
                p.snr_cdb     = static_cast<int16_t>(st.snr_db.load()   * 100.f);
                p.tx_kbps     = static_cast<uint16_t>(st.tx_kbps.load());
                p.rx_kbps     = static_cast<uint16_t>(st.rx_kbps.load());
                p.tx_duty_pct = static_cast<uint8_t>(st.tx_duty_now.load() * 100.f);
                p.temp_cc     = static_cast<int16_t>(st.temp_c.load() * 100.f);
            },
            cfg_.telemetry_bind, cfg_.telemetry_port);
        telemetry_->start();
    }

    exporter_ = std::make_unique<StatsExporter>(mode_->stats(), cfg_.stats_interval_ms,
                                                cfg_.stats_path);
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
                // Per-node path, matching the SDR_STATS_FILE/SDR_SCAN_FILE
                // convention. This was hard-coded to /tmp/sdr_reload.json while
                // the monitor writes /tmp/sdr_reload_<node>.json, so live
                // tuning silently reloaded a stale file -- or none at all --
                // for every node except an unnamed default.
                const char* rp = std::getenv("SDR_RELOAD_FILE");
                const std::string reload_path = (rp && *rp) ? rp : "/tmp/sdr_reload.json";
                Config fresh = Config::fromFile(reload_path);
                applyLive(fresh.tx_atten_db,
                          fresh.freq_tx_mhz * 1e6,
                          fresh.freq_rx_mhz * 1e6, 0);
            } catch (const std::exception& e) {
                std::cerr << "[sdr] reload error: " << e.what() << "\n";
            }
        }
    }

    exporter_->stop();
    if (telemetry_) telemetry_->stop();
    std::cout << "[sdr] Stopped.\n";
}

void Controller::applyLive(double atten_db, double freq_tx_hz,
                            double freq_rx_hz, int /*mod_code*/) {
    if (atten_db >= 0.0)  radio_->setTxAttenuation(atten_db);
    if (freq_tx_hz > 0.0) radio_->setTxFrequency(freq_tx_hz);
    if (freq_rx_hz > 0.0) radio_->setRxFrequency(freq_rx_hz);

    // Keep cfg_ in step with the hardware. Telemetry reports these fields, and
    // reporting the STARTUP value after a live retune makes `sdrctl status`
    // state the opposite of what the radio is doing -- which is worse than not
    // reporting them, because it is believed.
    if (atten_db >= 0.0)  cfg_.tx_atten_db = atten_db;
    if (freq_tx_hz > 0.0) cfg_.freq_tx_mhz = freq_tx_hz / 1e6;
    if (freq_rx_hz > 0.0) cfg_.freq_rx_mhz = freq_rx_hz / 1e6;
}

} // namespace sdr
