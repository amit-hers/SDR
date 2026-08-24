#pragma once
#include <atomic>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>
#include "../dsp/FFTSpectrum.hpp"

namespace sdr {

struct PeerInfo {
    float    rssi_dbm   {-100.f};
    float    snr_db     {0.f};
    uint64_t frames_rx  {0};
    uint64_t last_seen_ms {0};
};

struct LinkStats {
    // Frame counters
    std::atomic<uint64_t> frames_tx       {0};
    std::atomic<uint64_t> frames_rx_good  {0};
    std::atomic<uint64_t> frames_rx_bad   {0};
    std::atomic<uint64_t> dropped         {0};
    std::atomic<uint64_t> fec_corrected   {0};
    std::atomic<uint64_t> fec_uncorrectable{0};

    // ARQ counters (BridgeMode, when cfg.arq is enabled)
    std::atomic<uint64_t> arq_acked       {0};
    std::atomic<uint64_t> arq_retransmits {0};
    std::atomic<uint64_t> arq_dropped     {0};

    // RX pipeline funnel -- where frames are lost between "energy seen on
    // air" and "frame delivered". bursts_detected counts candidate windows
    // BurstDetector found; bursts_demodulated counts those that yielded a
    // CRC-good frame. A large gap means the demod chain is failing on real
    // bursts; a small bursts_detected relative to the peer's frames_tx means
    // energy isn't being seen at all (missed reception or detector threshold).
    std::atomic<uint64_t> bursts_detected    {0};
    std::atomic<uint64_t> bursts_demodulated {0};

    // Fraction of RX-thread wall time actually spent inside rxPull(). Time
    // spent demodulating is time NOT spent listening, and any burst that
    // arrives while we're busy is lost outright -- so a low value here caps
    // frame delivery no matter how good the DSP is.
    std::atomic<uint64_t> rx_pull_us   {0};
    std::atomic<uint64_t> rx_total_us  {0};
    // Channel occupancy: samples inside a detected burst, over samples seen.
    //
    // This is what "duty" was always meant to convey and never did --
    // rx_duty_pct is capture-thread utilisation, and was moreover computed
    // from two timestamps bracketing the same code, so it read ~100%
    // unconditionally. Conclusions of the form "duty is 100%, the channel is
    // saturated" were reading a constant.
    // Instantaneous TX duty, measured over the last stat tick rather than
    // over process lifetime. The lifetime average is diluted by startup and
    // teardown -- it read 47.8% while the transmitter was actually on air
    // ~92% of the time during the active window, which is the number the
    // duty LIMIT is supposed to bound.
    std::atomic<float>    tx_duty_now {0.f};
    std::atomic<float>    tx_duty_peak{0.f};
    std::atomic<uint64_t> rx_burst_samples {0};
    std::atomic<uint64_t> rx_seen_samples  {0};

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

    // Device telemetry
    std::atomic<float>    temp_c   {0.f};

    // Uptime
    std::atomic<uint64_t> uptime_s {0};

    // Peer table — keyed by node_id, updated on every decoded frame
    mutable std::mutex                   peers_mu;
    std::map<uint32_t, PeerInfo>         peers;

    void updatePeer(uint32_t node_id, float rssi, float snr);

    std::string toJSON() const;

    LinkStats() = default;
    LinkStats(const LinkStats&) = delete;
    LinkStats& operator=(const LinkStats&) = delete;
};

} // namespace sdr
