#pragma once
#include <complex>
#include <vector>
#include <liquid/liquid.h>

namespace sdr {

// Costas-loop carrier phase/frequency recovery (wraps liquid-dsp nco_crcf PLL).
class CostasLoop {
public:
    explicit CostasLoop(float loop_bw = 0.04f);
    ~CostasLoop();
    CostasLoop(const CostasLoop&)            = delete;
    CostasLoop& operator=(const CostasLoop&) = delete;

    std::complex<float> process(std::complex<float> in);
    void                process(const std::vector<std::complex<float>>& in,
                                std::vector<std::complex<float>>& out);
    float               phaseError() const { return phase_err_; }
    void                reset();

private:
    nco_crcf nco_      {nullptr};
    float    phase_err_{0.f};
};

} // namespace sdr
