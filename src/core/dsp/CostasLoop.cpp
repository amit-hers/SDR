#include "sdr/dsp/CostasLoop.hpp"
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

CostasLoop::CostasLoop(float loop_bw) {
    nco_ = nco_crcf_create(LIQUID_NCO);
    if (!nco_) throw std::runtime_error("CostasLoop: nco_crcf_create failed");
    nco_crcf_pll_set_bandwidth(nco_, loop_bw);
}

CostasLoop::~CostasLoop() {
    if (nco_) nco_crcf_destroy(nco_);
}

std::complex<float> CostasLoop::process(std::complex<float> in) {
    liquid_float_complex lin  = to_lc(in);
    liquid_float_complex lout{};
    nco_crcf_mix_down(nco_, lin, &lout);

    std::complex<float> out = from_lc(lout);
    float I = out.real(), Q = out.imag();
    float err = (I > 0.f ? 1.f : -1.f) * Q
              - (Q > 0.f ? 1.f : -1.f) * I;
    phase_err_ = err;
    nco_crcf_pll_step(nco_, err);
    nco_crcf_step(nco_);
    return out;
}

void CostasLoop::process(const std::vector<std::complex<float>>& in,
                          std::vector<std::complex<float>>& out) {
    out.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        out[i] = process(in[i]);
}

void CostasLoop::reset() {
    nco_crcf_reset(nco_);
    phase_err_ = 0.f;
}

} // namespace sdr
