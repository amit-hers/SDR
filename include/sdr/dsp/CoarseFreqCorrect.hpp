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
// carrier offset. Finding that tone and mixing it out collapses the offset
// down to a small residual that CostasLoop can then track natively.
//
// NOTE: an earlier version of this comment claimed validation to ~500 kHz.
// That was never true of this implementation -- the search is a bounded
// grid, and its range has always been about +/-49 kHz (it was implicit in a
// length-derived step size rather than stated). Offsets beyond MAX_CFO_HZ
// alias to a wrong answer rather than failing loudly. That is ample here:
// two free-running 0.5 ppm TCXOs at 434 MHz differ by only ~0.4 kHz.
class CoarseFreqCorrect {
public:
    // Search grid, in absolute Hz, for the CFO (not the doubled tone).
    //
    // These used to be derived from the buffer length, which meant the
    // search range and resolution silently changed with how much data was
    // handed in -- so restricting the analysis region would have quietly
    // altered acquisition behaviour as a side effect. Pinning them here
    // decouples "how much signal do we look at" (a cost decision) from
    // "what offsets can we find, how finely" (a behaviour decision).
    // The values match what the previous length-derived formula produced
    // for a full-size window: +/-48.8 kHz range at 244 Hz resolution.
    static constexpr double MAX_CFO_HZ    = 50e3;
    static constexpr double CFO_STEP_HZ   = 250.0;
    // Samples used for the estimate when a burst region is given. More is
    // not better once the acquisition section is covered: the search costs
    // one complex multiply per grid point per sample, so this is directly
    // proportional to CPU.
    //
    // The acquisition section is 400 BPSK symbols = 1600 samples at
    // RRC_SPS=4, and the burst window carries ~512 samples of pre-burst
    // margin ahead of it, so ~4k covers the preamble and sync word with
    // room to spare. The offset is a property of the two oscillators, not
    // of any one frame, so an estimate taken from the front of the window
    // is equally valid for every frame inside it.
    static constexpr size_t MAX_EST_SAMPLES = 4096;

    // Cap for the whole-buffer form, which has no burst region to aim at
    // and so must sample across everything (strided) to avoid being blind
    // to a burst sitting late in the buffer.
    static constexpr size_t MAX_SCAN_SAMPLES = 16384;

    // Estimates the CFO in `buf` and rotates it out in-place.
    // Returns the estimated offset in Hz (for logging/telemetry).
    static double apply(std::vector<std::complex<float>>& buf, double sample_rate);

    // Same, but estimates from only [est_begin, est_end) while still
    // correcting the whole buffer.
    //
    // The caller (the RX chain) extends its window to a full maximum-size
    // frame so a clipped burst detection can't truncate a real frame -- but
    // the *estimate* does not need those extra samples, and they are almost
    // entirely noise. Feeding the estimator just the detected burst is both
    // cheaper and better conditioned; the correction still has to cover
    // every sample that will later be demodulated.
    static double apply(std::vector<std::complex<float>>& buf, double sample_rate,
                        size_t est_begin, size_t est_end);

    // Selects the estimator. GRID is the bounded correlation search (the
    // original); FFT transforms the squared signal once and takes the peak
    // bin, which is O(N log N) instead of O(grid_points * N).
    enum class Method { GRID, FFT };
    static void setMethod(Method m) { method_ = m; }
    static Method method() { return method_; }

private:
    static Method method_;
    static double estimateGrid(const std::vector<std::complex<double>>& sq,
                               double eff_rate);
    static double estimateFft(const std::vector<std::complex<double>>& sq,
                              double eff_rate);
    static double applyImpl(std::vector<std::complex<float>>& buf, double sample_rate,
                            size_t est_begin, size_t est_end,
                            size_t max_samples, bool strided);
};

} // namespace sdr
