#include "sdr/hardware/PlutoSDR.hpp"
#include <stdexcept>
#include <cstring>
#include <cmath>

namespace sdr {

PlutoSDR::~PlutoSDR() {
    if (tx_buf_) iio_buffer_destroy(tx_buf_);
    if (rx_buf_) iio_buffer_destroy(rx_buf_);
    if (ctx_)    iio_context_destroy(ctx_);
}

std::unique_ptr<PlutoSDR> PlutoSDR::connect(const std::string& ip) {
    auto p = std::unique_ptr<PlutoSDR>(new PlutoSDR());

    p->ctx_ = iio_create_network_context(ip.c_str());
    if (!p->ctx_)
        throw std::runtime_error("PlutoSDR: cannot connect to " + ip);

    p->phy_    = iio_context_find_device(p->ctx_, "ad9361-phy");
    p->tx_dev_ = iio_context_find_device(p->ctx_, "cf-ad9361-dds-core-lpc");
    p->rx_dev_ = iio_context_find_device(p->ctx_, "cf-ad9361-lpc");

    if (!p->phy_ || !p->tx_dev_ || !p->rx_dev_)
        throw std::runtime_error("PlutoSDR: AD9361 devices not found");

    // Local oscillators
    p->lo_tx_ = iio_device_find_channel(p->phy_, "altvoltage1", true);
    p->lo_rx_ = iio_device_find_channel(p->phy_, "altvoltage0", true);

    // PHY channels
    p->tx_ch_ = iio_device_find_channel(p->phy_, "voltage0", true);
    p->rx_ch_ = iio_device_find_channel(p->phy_, "voltage0", false);

    if (!p->lo_tx_ || !p->lo_rx_ || !p->tx_ch_ || !p->rx_ch_)
        throw std::runtime_error("PlutoSDR: PHY channels not found");

    // IQ baseband channels
    p->tx_i_ = iio_device_find_channel(p->tx_dev_, "voltage0", true);
    p->tx_q_ = iio_device_find_channel(p->tx_dev_, "voltage1", true);
    p->rx_i_ = iio_device_find_channel(p->rx_dev_, "voltage0", false);
    p->rx_q_ = iio_device_find_channel(p->rx_dev_, "voltage1", false);

    if (!p->tx_i_ || !p->tx_q_ || !p->rx_i_ || !p->rx_q_)
        throw std::runtime_error("PlutoSDR: IQ channels not found");

    p->temp_ch_ = iio_device_find_channel(p->phy_, "temp0", false);

    iio_channel_enable(p->tx_i_);
    iio_channel_enable(p->tx_q_);
    iio_channel_enable(p->rx_i_);
    iio_channel_enable(p->rx_q_);

    // Create buffers
    p->tx_buf_ = iio_device_create_buffer(p->tx_dev_, p->buf_sz_, false);
    p->rx_buf_ = iio_device_create_buffer(p->rx_dev_, p->buf_sz_, false);

    if (!p->tx_buf_ || !p->rx_buf_)
        throw std::runtime_error("PlutoSDR: buffer allocation failed");

    return p;
}

void PlutoSDR::setTxFrequency(double hz) {
    iio_channel_attr_write_longlong(lo_tx_, "frequency",
                                    static_cast<long long>(hz));
}

void PlutoSDR::setRxFrequency(double hz) {
    iio_channel_attr_write_longlong(lo_rx_, "frequency",
                                    static_cast<long long>(hz));
}

void PlutoSDR::setTxAttenuation(double db) {
    // "hardwaregain" is a fractional-dB attribute (0.25dB steps on AD9361/63),
    // not millidB despite some vendor docs -- confirmed via readback.
    iio_channel_attr_write_double(tx_ch_, "hardwaregain", -db);
}

void PlutoSDR::setSampleRate(long long sps) {
    // The AD9361/9363 kernel driver validates the requested rate against the
    // digital interface's actual clock ceiling (e.g. CMOS-mode boards top
    // out well below the 61.44 MSPS LVDS spec) and silently keeps the prior
    // rate on rejection — these calls previously ignored that, so a request
    // for an unachievable rate left the whole DSP chain's oversampling-ratio
    // assumption (RRC_SPS) silently wrong. Fail loudly instead.
    int rx_ret = iio_channel_attr_write_longlong(rx_ch_, "sampling_frequency", sps);
    int tx_ret = iio_channel_attr_write_longlong(tx_ch_, "sampling_frequency", sps);
    if (rx_ret < 0 || tx_ret < 0)
        throw std::runtime_error(
            "PlutoSDR: setSampleRate(" + std::to_string(sps) +
            ") rejected by driver (rx=" + std::to_string(rx_ret) +
            " tx=" + std::to_string(tx_ret) +
            ") -- requested rate likely exceeds this board's digital "
            "interface ceiling (CMOS-mode boards: ~30.72 MSPS, not 61.44)");
}

void PlutoSDR::setBandwidth(long long hz) {
    int rx_ret = iio_channel_attr_write_longlong(rx_ch_, "rf_bandwidth", hz);
    int tx_ret = iio_channel_attr_write_longlong(tx_ch_, "rf_bandwidth", hz);
    if (rx_ret < 0 || tx_ret < 0)
        throw std::runtime_error(
            "PlutoSDR: setBandwidth(" + std::to_string(hz) +
            ") rejected by driver (rx=" + std::to_string(rx_ret) +
            " tx=" + std::to_string(tx_ret) + ")");
}

void PlutoSDR::setGainMode(const std::string& mode) {
    iio_channel_attr_write(rx_ch_, "gain_control_mode", mode.c_str());
}

void PlutoSDR::setManualGain(int db) {
    iio_channel_attr_write_longlong(rx_ch_, "hardwaregain",
                                    static_cast<long long>(db));
}

int PlutoSDR::txPush(const int16_t* iq, size_t n_pairs) {
    auto* p   = static_cast<int16_t*>(iio_buffer_start(tx_buf_));
    auto* end = static_cast<int16_t*>(iio_buffer_end(tx_buf_));
    size_t cap = static_cast<size_t>(end - p) / 2;
    size_t n   = (n_pairs < cap) ? n_pairs : cap;
    std::memcpy(p, iq, n * 2 * sizeof(int16_t));
    // iio_buffer_push transmits the WHOLE allocated buffer regardless of how
    // much we just wrote — zero the remainder so a short burst (the common
    // case: one frame is far smaller than the buffer) is followed by
    // silence, not whatever stale data was left over from the previous call.
    if (n < cap)
        std::memset(p + n * 2, 0, (cap - n) * 2 * sizeof(int16_t));
    iio_buffer_push(tx_buf_);
    return static_cast<int>(n);
}

int PlutoSDR::rxPull(int16_t* iq, size_t n_pairs) {
    ssize_t nb = iio_buffer_refill(rx_buf_);
    if (nb < 0) return -1;
    auto* p   = static_cast<int16_t*>(iio_buffer_start(rx_buf_));
    auto* end = static_cast<int16_t*>(iio_buffer_end(rx_buf_));
    size_t avail = static_cast<size_t>(end - p) / 2;
    size_t n     = (n_pairs < avail) ? n_pairs : avail;
    std::memcpy(iq, p, n * 2 * sizeof(int16_t));
    return static_cast<int>(n);
}

float PlutoSDR::getRSSI() const {
    double val = 0.0;
    iio_channel_attr_read_double(rx_ch_, "rssi", &val);
    return static_cast<float>(-val);   // AD9363 reports positive dB; RSSI is negative
}

float PlutoSDR::getTemp() const {
    if (!temp_ch_) return 0.f;
    long long raw = 0;
    iio_channel_attr_read_longlong(temp_ch_, "input", &raw);
    return static_cast<float>(raw) / 1000.f;  // millideg → °C
}

DeviceInfo PlutoSDR::getInfo() const {
    DeviceInfo info;
    info.temp_c = getTemp();
    if (const char* v = iio_context_get_attr_value(ctx_, "hw_model"))
        info.firmware = v;
    if (const char* v = iio_context_get_attr_value(ctx_, "hw_serial"))
        info.serial = v;
    return info;
}

} // namespace sdr
