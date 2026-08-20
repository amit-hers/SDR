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

    // Seed the loop with an externally acquired phase/frequency estimate.
    //
    // On short bursts the loop cannot pull in from scratch -- and with an
    // alternating (suppressed-carrier) preamble it has nothing to pull in on
    // anyway. DataAidedSync measures phase and frequency directly from the
    // known preamble+sync; seeding those here turns the loop into a *tracker*
    // that only has to follow residual drift, instead of an acquirer. That
    // matters because a one-shot estimate extrapolated across a whole frame
    // accumulates enough phase error to flip bits well before the CRC.
    //
    // `freq_per_sym` is radians per symbol.
    void seed(float phase, float freq_per_sym);

private:
    nco_crcf nco_      {nullptr};
    float    phase_err_{0.f};
};

} // namespace sdr
