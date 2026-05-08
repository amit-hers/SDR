#include "sdr/dsp/AGC.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace sdr {

static inline liquid_float_complex to_lc(std::complex<float> c) {
    liquid_float_complex lc;
    std::memcpy(&lc, &c, sizeof(lc));
    return lc;
}
static inline std::complex<float> from_lc(liquid_float_complex lc) {
    std::complex<float> c;
    std::memcpy(&c, &lc, sizeof(c));
    return c;
}

AGC::AGC() {
    agc_ = agc_crcf_create();
    if (!agc_) throw std::runtime_error("AGC: agc_crcf_create failed");
    agc_crcf_set_bandwidth(agc_, 0.001f);
    agc_crcf_set_signal_level(agc_, 1.f);
}

AGC::~AGC() {
    if (agc_) agc_crcf_destroy(agc_);
}

std::complex<float> AGC::process(std::complex<float> in) {
    liquid_float_complex lin  = to_lc(in);
    liquid_float_complex lout{};
    agc_crcf_execute(agc_, lin, &lout);
    return from_lc(lout);
}

void AGC::process(std::vector<std::complex<float>>& buf) {
    for (auto& s : buf) s = process(s);
}

float AGC::rssi_dbm() const {
    float level = agc_crcf_get_signal_level(agc_);
    return 20.f * std::log10(level + 1e-12f) - 30.f;
}

void AGC::reset() {
    agc_crcf_reset(agc_);
}

} // namespace sdr
