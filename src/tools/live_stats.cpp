// live_stats.cpp — reads real IQ from PlutoSDR and writes /tmp/sdr_stats.json
// No TAP/bridge needed, runs without sudo.
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/dsp/FFTSpectrum.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <cmath>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <algorithm>

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run.store(false); }

static std::string fmtf(float v, int p = 1) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(p) << v;
    return o.str();
}

int main(int argc, char** argv) {
    const std::string ip   = (argc > 1) ? argv[1] : "192.168.2.1";
    const double      freq = (argc > 2) ? std::stod(argv[2]) * 1e6 : 434e6;
    const int         bw   = (argc > 3) ? std::stoi(argv[3]) : 10;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    std::cout << "[live_stats] Connecting to " << ip << "...\n";
    std::unique_ptr<sdr::PlutoSDR> radio;
    try {
        radio = sdr::PlutoSDR::connect(ip);
        radio->setRxFrequency(freq);
        radio->setTxFrequency(freq + 5e6);   // TX somewhere harmless
        radio->setBandwidth(static_cast<long long>(bw) * 1'000'000LL);
        radio->setSampleRate(static_cast<long long>(bw) * 1'000'000LL * 4);
        radio->setGainMode("fast_attack");
        radio->setTxAttenuation(89);         // TX fully attenuated — we're only RX
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "[live_stats] Connected. RX=" << freq/1e6 << " MHz, BW=" << bw << " MHz\n"
              << "[live_stats] Writing /tmp/sdr_stats.json every 200ms. Ctrl-C to stop.\n";

    sdr::FFTSpectrum fft;
    fft.center_hz   = freq;
    fft.sample_rate = bw * 1e6 * 4;

    std::vector<int16_t> hw_buf(2048 * 2);

    uint64_t uptime_ticks = 0;
    uint64_t frames_rx    = 0;

    // Exponential moving average for RSSI smoothing
    float rssi_ema = -90.f;
    float snr_ema  =   0.f;
    float temp_c   = radio->getTemp();
    int   temp_ctr = 0;

    while (g_run.load()) {
        auto t0 = std::chrono::steady_clock::now();

        // Pull IQ samples
        int n = radio->rxPull(hw_buf.data(), 2048);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Convert int16 → complex float, feed FFT
        std::vector<std::complex<float>> iq(static_cast<size_t>(n));
        double power_sum = 0;
        for (int i = 0; i < n; ++i) {
            float I = hw_buf[i * 2]     / 32768.f;
            float Q = hw_buf[i * 2 + 1] / 32768.f;
            iq[static_cast<size_t>(i)] = {I, Q};
            power_sum += I*I + Q*Q;
        }
        fft.accumulate(iq.data(), n);

        // Hardware RSSI from AD9363 register (already calibrated in dBm)
        float rssi = radio->getRSSI();

        // Latch spectrum once per FFT window; reuse for SNR and JSON
        static std::array<float, sdr::FFTSpectrum::BINS> last_spec;
        static bool spec_valid = false;
        if (fft.ready()) {
            last_spec   = fft.get();   // clears ready flag — do this once only
            spec_valid  = true;
        }

        // SNR: spectrum peak vs median noise floor
        float snr = 0.f;
        if (spec_valid) {
            std::vector<float> tmp(last_spec.begin(), last_spec.end());
            std::sort(tmp.begin(), tmp.end());
            float noise_floor = tmp[tmp.size() / 2];
            float peak_power  = tmp.back();
            snr = peak_power - noise_floor;
        }

        // EMA smoothing (α=0.15)
        rssi_ema = rssi_ema * 0.85f + rssi * 0.15f;
        snr_ema  = snr_ema  * 0.85f + snr  * 0.15f;

        // Modulation from SNR
        int cur_mod = 1;
        if      (snr_ema >= 24) cur_mod = 4;
        else if (snr_ema >= 15) cur_mod = 3;
        else if (snr_ema >=  9) cur_mod = 2;

        // Read temperature every 5 s (~25 iterations at 200ms)
        if (++temp_ctr >= 25) {
            temp_c  = radio->getTemp();
            temp_ctr = 0;
        }

        ++frames_rx;
        ++uptime_ticks;

        // Build spectrum JSON using latched last_spec
        std::ostringstream spec_ss;
        spec_ss << "[";
        for (int i = 0; i < sdr::FFTSpectrum::BINS; ++i) {
            float v = spec_valid ? last_spec[static_cast<size_t>(i)] : rssi_ema - 10.f;
            spec_ss << fmtf(v);
            if (i < sdr::FFTSpectrum::BINS - 1) spec_ss << ",";
        }
        spec_ss << "]";

        // Write JSON atomically (write to tmp, rename)
        {
            std::ofstream f("/tmp/sdr_stats.json.tmp");
            f << "{"
              << "\"frames_tx\":0,"
              << "\"frames_rx_good\":" << frames_rx << ","
              << "\"frames_rx_bad\":0,"
              << "\"dropped\":0,"
              << "\"fec_corrected\":0,"
              << "\"fec_uncorrectable\":0,"
              << "\"bytes_tx\":0,"
              << "\"bytes_rx\":" << (frames_rx * 1400) << ","
              << "\"rssi_dbm\":"  << fmtf(rssi_ema) << ","
              << "\"snr_db\":"    << fmtf(snr_ema)  << ","
              << "\"tx_kbps\":0.0,"
              << "\"rx_kbps\":0.0,"
              << "\"dist_km\":0.0,"
              << "\"cur_mod\":"   << cur_mod << ","
              << "\"temp_c\":"    << fmtf(temp_c) << ","
              << "\"uptime_s\":"  << (uptime_ticks / 5) << ","
              << "\"spectrum\":"  << spec_ss.str()
              << "}";
        }
        std::rename("/tmp/sdr_stats.json.tmp", "/tmp/sdr_stats.json");

        // Sleep remainder of 200ms slot
        auto elapsed = std::chrono::steady_clock::now() - t0;
        auto remaining = std::chrono::milliseconds(200) - elapsed;
        if (remaining > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(remaining);
    }

    std::cout << "\n[live_stats] Stopped.\n";
    return 0;
}
