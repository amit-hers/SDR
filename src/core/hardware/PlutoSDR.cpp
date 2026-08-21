#include <iostream>
#include "sdr/hardware/PlutoSDR.hpp"
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>

namespace sdr {

namespace {

// Looks for a USB-backend context whose scan description advertises `serial`.
// Returns its URI, or an empty string if there isn't exactly one match.
// The USB backend avoids the TCP/IP stack that the network backend runs over
// the board's USB-ethernet gadget, which measurably raises the sustainable
// streaming rate (see PlutoSDR::connect).
std::string findUsbUriForSerial(const std::string& serial) {
    if (serial.empty()) return {};   // can't disambiguate without a serial

    iio_scan_context* scan = iio_create_scan_context(nullptr, 0);
    if (!scan) return {};

    iio_context_info** info = nullptr;
    ssize_t n = iio_scan_context_get_info_list(scan, &info);

    std::string match;
    int hits = 0;
    for (ssize_t i = 0; i < n; ++i) {
        std::string uri  = iio_context_info_get_uri(info[i]);
        std::string desc = iio_context_info_get_description(info[i]);
        if (uri.rfind("usb:", 0) != 0) continue;
        if (desc.find(serial) == std::string::npos) continue;
        match = uri;
        ++hits;
    }

    if (info) iio_context_info_list_free(info);
    iio_scan_context_destroy(scan);

    return (hits == 1) ? match : std::string{};
}

bool looksLikeUri(const std::string& s) {
    return s.rfind("usb:", 0) == 0 || s.rfind("ip:", 0) == 0
        || s.rfind("local:", 0) == 0 || s.find("://") != std::string::npos;
}

// The board's USB-ethernet gadget defaults to 192.168.2.x, so an address in
// that subnet means we're already talking over USB and the direct USB
// backend is strictly faster. Any *other* address is a real network path --
// most importantly the board's Gigabit RJ45, which sustains far more than
// USB 2.0 (measured RX duty at 48 MB/s: 96% over Ethernet vs 63% over USB).
// Silently "upgrading" that to USB would throw the Gigabit link away.
bool isUsbGadgetSubnet(const std::string& ip) {
    return ip.rfind("192.168.2.", 0) == 0;
}

} // namespace

PlutoSDR::~PlutoSDR() {
    if (tx_buf_) iio_buffer_destroy(tx_buf_);
    if (rx_buf_) iio_buffer_destroy(rx_buf_);
    if (ctx_)    iio_context_destroy(ctx_);
}

std::unique_ptr<PlutoSDR> PlutoSDR::connect(const std::string& id) {
    auto p = std::unique_ptr<PlutoSDR>(new PlutoSDR());

    if (looksLikeUri(id)) {
        // Explicit backend URI (e.g. "usb:1.56.5") -- use it verbatim.
        p->ctx_ = iio_create_context_from_uri(id.c_str());
        if (!p->ctx_)
            throw std::runtime_error("PlutoSDR: cannot open context " + id);
        p->transport_ = id;
    } else {
        // Plain IP. Connect over the network backend first, then transparently
        // upgrade to the USB backend for the same physical board when we can
        // identify it unambiguously by serial -- the network path runs TCP over
        // the board's USB-ethernet gadget and sustains a markedly lower
        // streaming rate, which starves the receiver of listening time.
        p->ctx_ = iio_create_network_context(id.c_str());
        if (!p->ctx_)
            throw std::runtime_error("PlutoSDR: cannot connect to " + id);
        p->transport_ = "ip:" + id;

        const char* serial = iio_context_get_attr_value(p->ctx_, "hw_serial");
        std::string usb_uri = isUsbGadgetSubnet(id)
                            ? findUsbUriForSerial(serial ? serial : "")
                            : std::string{};   // real network path -- keep it
        if (!usb_uri.empty()) {
            if (iio_context* usb = iio_create_context_from_uri(usb_uri.c_str())) {
                iio_context_destroy(p->ctx_);
                p->ctx_ = usb;
                p->transport_ = usb_uri;
            }
            // On failure just keep the working network context.
        }
    }

    std::printf("[sdr] %s: using transport %s\n", id.c_str(), p->transport_.c_str());

    p->phy_    = iio_context_find_device(p->ctx_, "ad9361-phy");
    p->tx_dev_ = iio_context_find_device(p->ctx_, "cf-ad9361-dds-core-lpc");
    p->rx_dev_ = iio_context_find_device(p->ctx_, "cf-ad9361-lpc");

    if (!p->phy_ || !p->tx_dev_ || !p->rx_dev_)
        throw std::runtime_error("PlutoSDR: AD9361 devices not found on "
                                 + p->transport_);

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

    // Create buffers. TX is deliberately much smaller than RX: a push costs
    // its full buffer length in airtime regardless of how much was written.
    p->tx_buf_ = iio_device_create_buffer(p->tx_dev_, p->tx_buf_sz_, false);
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

    // A write can succeed and still land on a different rate: the driver
    // quantises to what the clock tree can synthesise. RRC_SPS assumes the
    // sample rate is exactly 4x the symbol rate, so report the applied value
    // and complain if it is not what we asked for.
    long long rx_now = 0, tx_now = 0;
    iio_channel_attr_read_longlong(rx_ch_, "sampling_frequency", &rx_now);
    iio_channel_attr_read_longlong(tx_ch_, "sampling_frequency", &tx_now);
    std::cerr << "[sdr] sample rate requested " << sps
              << " -> applied rx=" << rx_now << " tx=" << tx_now << "\n";
    if (rx_now != sps || tx_now != sps)
        std::cerr << "[sdr] WARNING: applied sample rate differs from the "
                     "request; the DSP chain assumes exactly "
                  << sps << " (RRC_SPS oversampling), so timing recovery will "
                     "be off by the ratio.\n";
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
    if (n_pairs == 0) return 0;

    // Refill only once the previous one is used up; otherwise keep serving
    // from it, so consecutive reads are contiguous in time. iio_buffer_start()
    // stays valid until the next refill, which is exactly when we re-read it.
    if (rx_pos_ >= rx_avail_) {
        ssize_t nb = iio_buffer_refill(rx_buf_);
        if (nb < 0) return -1;
        auto* p   = static_cast<int16_t*>(iio_buffer_start(rx_buf_));
        auto* end = static_cast<int16_t*>(iio_buffer_end(rx_buf_));
        rx_avail_ = static_cast<size_t>(end - p) / 2;
        rx_pos_   = 0;
        if (rx_avail_ == 0) return 0;
    }

    auto* base = static_cast<int16_t*>(iio_buffer_start(rx_buf_));
    size_t left = rx_avail_ - rx_pos_;
    size_t n    = (n_pairs < left) ? n_pairs : left;
    std::memcpy(iq, base + rx_pos_ * 2, n * 2 * sizeof(int16_t));
    rx_pos_ += n;
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
