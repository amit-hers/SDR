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

    // Quadrant-slip prevention.
    //
    // A QPSK Costas loop has four stable lock points 90 degrees apart. Noise
    // can walk it into an adjacent one, and it then stays there: every
    // remaining symbol is rotated 90 degrees, which under Gray coding flips
    // one bit per symbol and corrupts the frame from the slip point to the
    // end. Measured at 2 MHz, 86-97% of corrupted bytes carry exactly that
    // signature (a one-bit-per-symbol-pair XOR mask -- 16 of 256 possible,
    // 6% by chance), in runs of 800-1048 bytes reaching the frame end.
    //
    // Narrowing the loop bandwidth cut the rate 44% but not the mechanism.
    // This bounds the NCO phase to +-limit around its seeded start, so the
    // loop physically cannot reach an adjacent lock point while retaining
    // full tracking authority inside its own basin. 0 disables.
    //
    // Only viable because DataAidedSync has already removed the bulk phase
    // ramp: the residual the loop must follow is small, so a +-45 degree
    // basin is ample. It would NOT be safe on a loop doing real acquisition.
    void setPhaseLimit(float rad) { phase_limit_ = rad; }

private:
    nco_crcf nco_      {nullptr};
    float    phase_err_{0.f};
    float    phase_limit_{0.f};
    float    phase_ref_{0.f};
};

} // namespace sdr
