#include <cstdlib>
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
    // Loop bandwidth ~ 1/settling-time in SAMPLES.
    //
    // 0.001 gives a ~1000-sample time constant. The AGC is reset per burst
    // and the acquisition section is only 400 BPSK symbols = 1600 samples at
    // 4 sps, so it reached the payload ~80% converged and bled a residual
    // gain error into the early payload: measured amplitude in the first
    // quarter of the payload was 0.875 (1 MHz) / 0.900 (2 MHz) of the rest,
    // lowering effective SNR exactly where CRC failures concentrate.
    //
    // The AD9363's own hardware AGC (slow_attack) does the coarse work, so
    // this loop only trims -- it can afford to be faster. Override with
    // SDR_AGC_BW to re-measure.
    //
    // Swept at 2 MHz: 0.001 -> amp ratio 0.890 (droop), 0.005 -> 0.9937,
    // 0.01 -> 0.9933, 0.03 -> 0.9920. 0.005 is the knee: it removes the droop
    // and nothing above it helps, with 0.03 starting to track the modulation
    // itself. Note the droop fix moved CRC only 94.5% -> 95.2% -- real, but
    // small, because hard-decision QPSK is insensitive to uniform amplitude.
    float agc_bw = 0.005f;
    if (const char* e = std::getenv("SDR_AGC_BW")) {
        float v = std::strtof(e, nullptr);
        if (v > 0.f && v < 0.5f) agc_bw = v;
    }
    agc_crcf_set_bandwidth(agc_, agc_bw);
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
