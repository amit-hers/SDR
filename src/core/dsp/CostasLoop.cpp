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

    // Keep the loop inside its own lock basin. Without this it can walk into
    // an adjacent 90-degree lock point and stay there, rotating every
    // subsequent symbol -- see setPhaseLimit().
    if (phase_limit_ > 0.f) {
        float ph = nco_crcf_get_phase(nco_);
        float d  = ph - phase_ref_;
        while (d >  3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        if (d >  phase_limit_) nco_crcf_set_phase(nco_, phase_ref_ + phase_limit_);
        else if (d < -phase_limit_) nco_crcf_set_phase(nco_, phase_ref_ - phase_limit_);
    }
    return out;
}

void CostasLoop::process(const std::vector<std::complex<float>>& in,
                          std::vector<std::complex<float>>& out) {
    out.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        out[i] = process(in[i]);
}

void CostasLoop::reset() {
    phase_ref_ = 0.f;
    nco_crcf_reset(nco_);
    phase_err_ = 0.f;
}

void CostasLoop::seed(float phase, float freq_per_sym) {
    phase_ref_ = phase;
    nco_crcf_set_phase(nco_, phase);
    nco_crcf_set_frequency(nco_, freq_per_sym);
    phase_err_ = 0.f;
}

} // namespace sdr
