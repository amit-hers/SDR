#pragma once
#include "sdr/framing/Frame.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include <string>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace sdr {

struct Config {
    // ── Operating mode ──────────────────────────────────────────────────────
    std::string mode        {"bridge"};   // bridge|mesh|p2p-tx|p2p-rx|scan

    // ── Hardware ────────────────────────────────────────────────────────────
    // Plain IP, or an explicit libiio backend URI ("usb:1.56.5") -- see
    // PlutoSDR::connect. The USB backend sustains a much higher streaming
    // rate than the network backend, so prefer it where possible.
    std::string pluto_ip    {"192.168.2.1"};
    double      freq_tx_mhz {434.0};
    double      freq_rx_mhz {439.0};
    // Sample rate is bw_mhz * samples_per_symbol, and every sample is 4 bytes over
    // USB. USB 2.0 tops out near 35 MB/s in practice, so anything above
    // ~2 MHz here starves the receiver of listening time: measured RX duty
    // cycle is 99% at 1 MHz, 95% at 2 MHz (USB backend), but only 38% at
    // 5 MHz and 27% at 5 MHz over the network backend. Bursts arriving while
    // the receiver isn't listening are simply never seen, so a "faster"
    // setting here yields *fewer* delivered frames, not more.
    int         bw_mhz      {1};          // 1|2|5|10|20 (1-2 recommended)
    // Two samples/symbol halves host-interface traffic and is intended for
    // high-throughput links. Four remains the validated compatibility mode.
    int         samples_per_symbol {RRC_SPS}; // 2|4
    double      tx_atten_db {10.0};       // 0-89 dB, 0.25dB steps
    std::string gain_mode   {"fast_attack"};
    // Payload modulation for TRANSMIT only; the receive side always takes the
    // payload scheme from the frame header. Acquisition (preamble/sync/header)
    // is fixed BPSK regardless -- see Frame.hpp.
    //
    // "AUTO" is NOT a wire scheme and is currently refused: link adaptation
    // needs an explicit negotiation between the two nodes, and the previous
    // implementation (each node picking from its own local RSSI) could not
    // produce agreement. Defaults to BPSK, the validated configuration.
    std::string modulation  {"BPSK"};     // BPSK|QPSK (16QAM|64QAM untested on air)

    // Fraction of the air this node may occupy with its own transmissions.
    //
    // The receiver acquires per burst: it needs quiet between bursts to
    // estimate a noise floor and to tell where one burst ends. Measured on
    // the live link, an unthrottled transmitter under load fills the channel
    // to 100% occupancy, at which point burst detection degenerates and
    // frame recovery collapses to ~3% -- while the same receiver recovers
    // ~92% from captures at 57-63% occupancy. There is no medium access
    // control here, so the limit has to be self-imposed.
    //
    // 0 (or >= 1) disables throttling.
    double      tx_duty_max  {0.65};

    // Defer transmitting while the peer is heard on the air.
    //
    // Per-node duty limiting alone does not work: two independently
    // scheduled 65% transmitters still fill the channel, because each one's
    // bursts land in the other's gaps (measured 96% occupancy with both
    // active, 70% with one silenced -- and 13x the goodput in the latter).
    // The gaps have to be ones both nodes agree on.
    //
    // This node receives on the frequency the peer transmits on, so its own
    // energy detector is already a peer-activity sensor; nothing new has to
    // be measured.
    // Samples pulled per capture buffer, and how many may queue for the DSP
    // thread.
    //
    // These set the real-time deadline: the DSP thread must finish a buffer
    // within buffer_samples / sample_rate. At 2 MHz (8 MSPS) a 256K buffer is
    // only 32.8 ms, against a measured ~21 ms of DSP work -- too little
    // margin, and 28% of buffers were dropped. Smaller buffers spread the
    // same work across more, shorter deadlines; a deeper queue absorbs
    // bursts at the cost of latency.
    //
    // The trade-off is that a burst near the end of a buffer cannot be
    // extended past it, so smaller buffers truncate more frames.
    // Analog filter width as a multiple of the symbol rate. RRC at rolloff
    // 0.35 occupies 1.35x in theory; too narrow clips the pulse skirts the
    // matched filter needs, too wide admits extra noise. Swept on hardware
    // because the AD9363's filter is not ideal and the best point is not
    // necessarily the theoretical one.
    // Real-time scheduling for the capture and DSP threads.
    //
    // At 2 MHz the DSP thread uses only ~64% of its per-buffer budget on
    // average, yet identical runs vary 3.9x in goodput and 62x in dropped
    // buffers. That pattern -- marginal average load, wild variance -- is
    // scheduling jitter, not throughput. SCHED_FIFO removes preemption by
    // ordinary tasks; pinning keeps the capture and DSP threads off each
    // other's core and preserves cache locality.
    //
    // 0 disables (ordinary SCHED_OTHER). Needs CAP_SYS_NICE / root.
    int         rt_priority  {0};
    bool        pin_cores    {false};

    // "fft" (default) or "grid". The FFT estimator is O(N log N) against the
    // grid search's O(points*N): measured 0.337 vs 5.463 ms per call, 16x,
    // with worst-case error 71 Hz -- far inside what the downstream carrier
    // recovery absorbs.
    std::string cfo_method {"fft"};

    double      rx_bw_factor {1.4};

    int         rx_buffer_samples {262144};
    int         rx_queue_depth    {8};

    bool        carrier_sense       {true};
    int         carrier_sense_hold_ms {25};   // stay off after hearing energy
    int         carrier_sense_max_defer_ms {60}; // never starve the transmitter

    // ── Bridge / mesh interface ──────────────────────────────────────────────
    std::string tap_iface    {"sdr0"};
    std::string bridge_iface {"br0"};
    std::string lan_iface    {"eth0"};
    int         tap_mtu      {1386};      // MAX_PAYLOAD(1400) - Ethernet header(14)

    // ── Security ─────────────────────────────────────────────────────────────
    bool        encrypt       {false};
    bool        fec           {false};
    std::string aes_key_hex;              // 64 hex chars = 32 bytes

    // ── Burst detection (RX) ─────────────────────────────────────────────────
    // Tuning knobs for BurstDetector. Defaults match BurstDetector::Config;
    // exposed here because the right threshold depends on the noise floor of
    // the deployment, and because `bursts_detected` vs `bursts_demodulated`
    // in the stats makes the effect directly measurable.
    int         burst_block      {256};   // samples per power block
    double      burst_threshold  {3.0};   // multiple of median block power
    int         burst_margin     {512};   // context kept either side
    int         burst_merge_gap  {512};   // merge windows closer than this
    double      burst_noise_q    {0.20};  // quantile treated as noise floor

    // ── ARQ (reliable delivery, BridgeMode only) ─────────────────────────────
    bool        arq             {false};
    int         arq_window      {16};
    int         arq_timeout_ms  {80};
    int         arq_max_retries {5};

    // ── System ───────────────────────────────────────────────────────────────
    int         stats_interval_ms {1000};
    // Minimum gap between FFT spectrum updates. The spectrum feeds the
    // monitor UI only -- it contributes nothing to decoding -- yet it ran on
    // every capture buffer and was measured at 73% of all CPU while the
    // receiver was hearing nothing but noise. A few updates per second is
    // ample for a display. 0 disables the spectrum entirely.
    int         spectrum_interval_ms {200};
    // Where the JSON stats snapshot is written. Configurable because two
    // nodes run on one host during link testing and would otherwise
    // overwrite each other's file.
    std::string stats_path        {"/tmp/sdr_stats.json"};
    int         monitor_port      {8080};
    std::string node_id           {"0x00000001"};

    // ── Channel scan (mode=scan) ─────────────────────────────────────────────
    double      scan_start_mhz {430.0};
    double      scan_step_mhz  {1.0};
    int         scan_n         {20};

    // Parsed AES key (populated by validate())
    std::array<uint8_t, 32> aes_key_bytes{};
    uint32_t                node_id_u32{1};

    // Load from JSON file; throws on error.
    static Config fromFile(const std::string& path);

    // Parse / validate fields. Throws std::invalid_argument on bad values.
    void validate();

    // Transmit payload modulation, resolved from `modulation`. Always a wire
    // scheme -- validate() rejects or downgrades anything else first.
    ModCode txModCode() const;
};

} // namespace sdr
