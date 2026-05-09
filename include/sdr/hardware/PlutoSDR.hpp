#pragma once
#include <iio.h>
#include <memory>
#include <string>
#include <stdexcept>

namespace sdr {

struct DeviceInfo {
    std::string serial;
    std::string firmware;
    float       temp_c{0.f};
};

class PlutoSDR {
public:
    ~PlutoSDR();
    PlutoSDR(const PlutoSDR&)            = delete;
    PlutoSDR& operator=(const PlutoSDR&) = delete;

    static std::unique_ptr<PlutoSDR> connect(const std::string& ip);

    void setTxFrequency(double hz);
    void setRxFrequency(double hz);
    void setTxAttenuation(int db);       // 0–89 dB (0 = max power)
    void setSampleRate(long long sps);   // 1e6–20e6
    void setBandwidth(long long hz);
    void setGainMode(const std::string& mode); // "fast_attack"|"slow_attack"|"manual"
    void setManualGain(int db);                // used when mode == "manual"

    // Returns samples written/read (IQ pairs, each 2×int16)
    int txPush(const int16_t* iq, size_t n_pairs);
    int rxPull(int16_t* iq, size_t n_pairs);

    float      getRSSI()  const;
    float      getTemp()  const;
    DeviceInfo getInfo()  const;

    static constexpr int    IQ_SCALE      = 2047;
    static constexpr size_t DEFAULT_BUF   = 1024 * 256;  // samples

private:
    explicit PlutoSDR() = default;

    iio_context* ctx_    {nullptr};
    iio_device*  phy_    {nullptr};
    iio_device*  tx_dev_ {nullptr};
    iio_device*  rx_dev_ {nullptr};
    iio_buffer*  tx_buf_ {nullptr};
    iio_buffer*  rx_buf_ {nullptr};
    iio_channel* lo_tx_  {nullptr};
    iio_channel* lo_rx_  {nullptr};
    iio_channel* tx_ch_  {nullptr};
    iio_channel* rx_ch_  {nullptr};
    iio_channel* rx_i_   {nullptr};
    iio_channel* rx_q_   {nullptr};
    iio_channel* tx_i_   {nullptr};
    iio_channel* tx_q_   {nullptr};
    iio_channel* temp_ch_{nullptr};
    size_t       buf_sz_ {DEFAULT_BUF};
};

} // namespace sdr
