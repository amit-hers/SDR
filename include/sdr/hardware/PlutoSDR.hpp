#pragma once
#include <iio.h>
#include <cstdint>
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
    // txPush transmits exactly n_pairs samples (via iio_buffer_push_partial),
    // so airtime is proportional to the data written rather than to the
    // buffer size. Batching still helps -- each push has fixed overhead --
    // but a short frame no longer costs a full buffer of airtime.
    int txPush(const int16_t* iq, size_t n_pairs);

    // Reads up to n_pairs samples, refilling the hardware buffer only when the
    // previous refill has been fully consumed. Successive small reads therefore
    // return *consecutive* samples rather than a fresh refill each time.
    //
    // This used to refill unconditionally and return only the first n_pairs,
    // silently discarding the rest -- so rxPull(buf, 8192) against a 262144-
    // sample buffer kept 3.1% of the air and threw away the other 96.9%. The
    // resulting stream looked continuous but was spliced, and no frame longer
    // than the request could survive it. Callers that pass DEFAULT_BUF (the
    // daemon's capture thread) are unaffected either way.
    int rxPull(int16_t* iq, size_t n_pairs);

    // IQ pairs per tx buffer push -- the batching target for callers.
    size_t txCapacity() const { return tx_buf_sz_; }

    // Transmit accounting. `pushed_pairs` is what the hardware actually
    // accepted, which is the only number that corresponds to airtime.
    struct TxStats {
        uint64_t pushes{0};
        uint64_t requested_pairs{0};
        uint64_t pushed_pairs{0};
        uint64_t short_pushes{0};
    };
    TxStats txStats() const {
        return { tx_pushes_, tx_req_pairs_, tx_pushed_pairs_, tx_short_pushes_ };
    }

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
    // Read cursor into the current rx refill, so partial reads consume the
    // buffer instead of discarding its tail.
    size_t       rx_avail_  {0};      // IQ pairs in the current refill
    size_t       rx_pos_    {0};      // IQ pairs already handed out
    // Set once if the transport rejects a partial push, so we stop retrying.
    bool         tx_partial_unsupported_ {false};
    uint64_t     tx_pushes_        {0};
    uint64_t     tx_req_pairs_     {0};
    uint64_t     tx_pushed_pairs_  {0};
    uint64_t     tx_short_pushes_  {0};
    std::string  transport_;   // libiio backend actually in use
};

} // namespace sdr
