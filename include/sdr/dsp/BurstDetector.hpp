#pragma once
#include <complex>
#include <vector>
#include <cstddef>

namespace sdr {

// Cheap energy-based burst detection over a raw rxPull batch. A real
// transmission occupies only a tiny fraction of a batch (bursts are sparse,
// no continuous carrier between frames) — feeding AGC/TimingSync/CostasLoop
// the whole mostly-silent batch continuously lets them adapt to whatever's
// dominant (the silence), not the brief real signal. Detecting and isolating
// the actual burst window(s) first, so the demod chain can be reset and run
// fresh on just that window, is standard burst-mode-radio practice.
class BurstDetector {
public:
    struct Window {
        size_t start;
        size_t end;   // exclusive
    };

    struct Config {
        size_t block_size  {256};  // samples per power-measurement block
        float  threshold_x {3.0f}; // multiple of the median block power
        size_t margin       {512}; // samples of context kept on each side
        size_t merge_gap     {512}; // merge windows separated by less than this
    };

    BurstDetector() = default;
    explicit BurstDetector(const Config& cfg) : cfg_(cfg) {}

    // Returns candidate burst regions in `buf` (empty if nothing crosses the
    // threshold — the common case, since most batches are pure silence/noise).
    std::vector<Window> detect(const std::vector<std::complex<float>>& buf) const;

private:
    Config cfg_;
};

} // namespace sdr
