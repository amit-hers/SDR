#pragma once
#include <atomic>
#include <array>
#include <string>
#include <cstdint>
#include "../dsp/FFTSpectrum.hpp"

namespace sdr {

struct LinkStats {
    // Frame counters
    std::atomic<uint64_t> frames_tx       {0};
    std::atomic<uint64_t> frames_rx_good  {0};
    std::atomic<uint64_t> frames_rx_bad   {0};
    std::atomic<uint64_t> dropped         {0};
    std::atomic<uint64_t> fec_corrected   {0};
    std::atomic<uint64_t> fec_uncorrectable{0};

    // Byte counters
    std::atomic<uint64_t> bytes_tx {0};
    std::atomic<uint64_t> bytes_rx {0};

    // Signal quality (updated each frame)
    std::atomic<float> rssi_dbm  {-100.f};
    std::atomic<float> snr_db    {0.f};
    std::atomic<float> tx_kbps   {0.f};
    std::atomic<float> rx_kbps   {0.f};
    std::atomic<float> dist_km   {0.f};

    // Current modulation (1=BPSK 2=QPSK 3=16QAM 4=64QAM)
    std::atomic<int> cur_mod {2};

    // Spectrum (updated ~5 Hz from stat thread); protected by spectrum_mu
    std::array<float, FFT_BINS> spectrum{};

    // Uptime
    std::atomic<uint64_t> uptime_s {0};

    std::string toJSON() const;

    LinkStats() = default;
    LinkStats(const LinkStats&) = delete;
    LinkStats& operator=(const LinkStats&) = delete;
};

} // namespace sdr
