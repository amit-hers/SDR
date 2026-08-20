#pragma once
#include <complex>
#include <vector>

namespace sdr {

// Blind coarse carrier-frequency-offset correction, applied before AGC and
// CostasLoop. CostasLoop's own pull-in range is narrow (~tens of kHz at
// typical loop bandwidths) and — critically — behaves non-monotonically
// past that range: some offsets far outside the nominal range still lock,
// others well within it don't, so widening its bandwidth alone can't fix
// this reliably. Two independent free-running TCXOs (no shared reference
// clock) can easily produce a combined offset in the tens-to-hundreds of
// kHz, especially at higher tuned frequencies (ppm scales with absolute
// frequency).
//
// This uses the squaring method: for a BPSK-modulated signal, x(t)^2
// removes the +-1 data modulation and leaves a residual tone at 2x the
// carrier offset. Finding that tone via FFT and mixing it out collapses
// any such offset down to a small residual that CostasLoop can then track
// natively. Validated synthetically up to ~500 kHz of offset (see
// dev notes / coarse_cfo_probe test); only breaks down close to 1 MHz.
class CoarseFreqCorrect {
public:
    // Estimates the CFO in `buf` and rotates it out in-place.
    // Returns the estimated offset in Hz (for logging/telemetry).
    static double apply(std::vector<std::complex<float>>& buf, double sample_rate);
};

} // namespace sdr
