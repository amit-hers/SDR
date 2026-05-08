#include "BridgeMode.hpp"
#include "sdr/framing/Frame.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <complex>

namespace sdr {

static constexpr int STAT_TICK = 100;   // ms

BridgeMode::BridgeMode(const Config& cfg, PlutoSDR& radio)
    : cfg_(cfg), radio_(radio)
{
    tap_ = TUNTAPDevice::create(cfg_.tap_iface, /*tap=*/true);
    tap_->setMTU(cfg_.tap_mtu);
    if (!cfg_.bridge_iface.empty())
        tap_->addToBridge(cfg_.bridge_iface, cfg_.lan_iface);

    amod_ = std::make_unique<AdaptiveModem>();
    if (cfg_.fec)     fec_ = std::make_unique<ReedSolomon>();
    if (cfg_.encrypt) aes_ = std::make_unique<AESCipher>(cfg_.aes_key_bytes.data());
    fft_ = std::make_unique<FFTSpectrum>();
}

BridgeMode::~BridgeMode() { stop(); }

void BridgeMode::start() {
    if (running_.exchange(true)) return;
    stats_.uptime_s.store(0);
    tx_thread_   = std::thread(&BridgeMode::txThread,   this);
    rx_thread_   = std::thread(&BridgeMode::rxThread,   this);
    stat_thread_ = std::thread(&BridgeMode::statThread, this);
}

void BridgeMode::stop() {
    running_.store(false);
    if (tx_thread_.joinable())   tx_thread_.join();
    if (rx_thread_.joinable())   rx_thread_.join();
    if (stat_thread_.joinable()) stat_thread_.join();
}

// ── TX thread ────────────────────────────────────────────────────────────────
void BridgeMode::txThread() {
    std::vector<uint8_t>             pkt(MAX_PAYLOAD);
    std::vector<std::complex<float>> iq_syms, iq_shaped;
    std::vector<int16_t>             iq_hw;
    RRCInterp interp;

    while (running_.load()) {
        ssize_t n = tap_->read(pkt.data(), pkt.size());
        if (n <= 0) continue;

        uint8_t flags = 0;
        if (cfg_.fec)     flags |= FL_FEC;
        if (cfg_.encrypt) flags |= FL_ENCRYPT;
        uint32_t seq = tx_seq_.fetch_add(1, std::memory_order_relaxed);
        ModCode  mod = amod_->currentModCode();

        Framer framer;
        auto frame = framer.encode(pkt.data(), static_cast<size_t>(n),
                                   flags, mod, mhzToBw(cfg_.bw_mhz),
                                   cfg_.node_id_u32, seq,
                                   fec_.get(), aes_.get());

        amod_->modulate(frame.data(), static_cast<int>(frame.size()), iq_syms);
        interp.process(iq_syms, iq_shaped);

        iq_hw.resize(iq_shaped.size() * 2);
        for (size_t i = 0; i < iq_shaped.size(); ++i) {
            iq_hw[i * 2]     = static_cast<int16_t>(iq_shaped[i].real() * 2047.f);
            iq_hw[i * 2 + 1] = static_cast<int16_t>(iq_shaped[i].imag() * 2047.f);
        }
        radio_.txPush(iq_hw.data(), iq_shaped.size());
        stats_.frames_tx.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_tx .fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    }
}

// ── RX thread ────────────────────────────────────────────────────────────────
void BridgeMode::rxThread() {
    std::vector<int16_t>             iq_hw(IQ_SAMPLES * 2);
    std::vector<std::complex<float>> iq_f, iq_dec, iq_timed, iq_syms;
    AGC agc; RRCDecim decim; TimingSync tsync; CostasLoop costas;
    Deframer deframer;

    while (running_.load()) {
        int n = radio_.rxPull(iq_hw.data(), IQ_SAMPLES);
        if (n <= 0) continue;

        iq_f.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            iq_f[static_cast<size_t>(i)] = {
                iq_hw[static_cast<size_t>(i) * 2]     / 2048.f,
                iq_hw[static_cast<size_t>(i) * 2 + 1] / 2048.f };

        fft_->accumulate(iq_f.data(), n);
        if (fft_->ready()) {
            auto& spec = fft_->get();
            for (size_t i = 0; i < spec.size(); ++i)
                stats_.spectrum[i] = spec[i];
        }

        agc.process(iq_f);                       // in-place
        decim.process(iq_f, iq_dec);             // downsample by RRC_SPS
        tsync.process(iq_dec, iq_timed);         // symbol timing
        costas.process(iq_timed, iq_syms);       // carrier recovery

        std::vector<uint8_t> bits;
        amod_->demodulate(iq_syms.data(), static_cast<int>(iq_syms.size()), bits);

        float rssi = agc.rssi_dbm();
        float snr  = rssi + 95.f;
        stats_.rssi_dbm.store(rssi, std::memory_order_relaxed);
        stats_.snr_db  .store(snr,  std::memory_order_relaxed);
        amod_->updateSNR(snr);
        stats_.cur_mod .store(static_cast<int>(amod_->currentModCode()),
                              std::memory_order_relaxed);

        for (uint8_t byte : bits) {
            auto result = deframer.push(byte, fec_.get(), aes_.get());
            if (!result) continue;
            ssize_t w = tap_->write(result->payload.data(), result->payload.size());
            if (w > 0) {
                stats_.frames_rx_good.fetch_add(1, std::memory_order_relaxed);
                stats_.bytes_rx.fetch_add(static_cast<uint64_t>(w),
                                          std::memory_order_relaxed);
                if (result->flags & FL_FEC)
                    stats_.fec_corrected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ── Stat thread ───────────────────────────────────────────────────────────────
void BridgeMode::statThread() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    uint64_t prev_tx = 0, prev_rx = 0;

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
        stats_.uptime_s.fetch_add(STAT_TICK / 1000, std::memory_order_relaxed);
    }
}

} // namespace sdr
