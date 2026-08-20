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

    // `id` is either a plain IP ("192.168.2.1") or an explicit libiio backend
    // URI ("usb:1.56.5"). For a plain IP the connection transparently upgrades
    // to the USB backend when the same board can be identified unambiguously
    // by serial -- the network backend runs TCP over the board's USB-ethernet
    // gadget and sustains a markedly lower streaming rate. Boards without a
    // programmed serial (`fw_setenv UniqueID ...`) can't be matched, so pass
    // their `usb:` URI explicitly to get the faster path.
    static std::unique_ptr<PlutoSDR> connect(const std::string& id);

    // Which libiio backend this instance actually ended up using.
    const std::string& transport() const { return transport_; }

    void setTxFrequency(double hz);
    void setRxFrequency(double hz);
    void setTxAttenuation(double db);    // 0–89 dB (0 = max power); AD9361 supports 0.25dB steps
    void setSampleRate(long long sps);   // 1e6–61.44e6 (HamGeek Pluto+)
    void setBandwidth(long long hz);
    void setGainMode(const std::string& mode); // "fast_attack"|"slow_attack"|"manual"
    void setManualGain(int db);                // used when mode == "manual"

    // Returns samples written/read (IQ pairs, each 2×int16).
    // NOTE: txPush transmits the WHOLE tx buffer (zero-padding whatever the
    // caller didn't fill), so airtime cost is txCapacity() regardless of
    // n_pairs. Batch frames up to txCapacity() before pushing, or most of
    // the air time is spent radiating zeros.
    int txPush(const int16_t* iq, size_t n_pairs);
    int rxPull(int16_t* iq, size_t n_pairs);

    // IQ pairs per tx buffer push -- the batching target for callers.
    size_t txCapacity() const { return tx_buf_sz_; }

    float      getRSSI()  const;
    float      getTemp()  const;
    DeviceInfo getInfo()  const;

    static constexpr int    IQ_SCALE      = 2047;
    static constexpr size_t DEFAULT_BUF   = 1024 * 256;  // rx samples
    // TX buffer is sized to hold one maximum-size frame (a 1400-byte payload
    // at BPSK is ~47k samples) with a little slack -- not the much larger rx
    // size, since every push costs its full length in airtime.
    static constexpr size_t TX_BUF        = 1024 * 64;   // tx samples

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
    size_t       buf_sz_    {DEFAULT_BUF};
    size_t       tx_buf_sz_ {TX_BUF};
    std::string  transport_;   // libiio backend actually in use
};

} // namespace sdr
