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
        float  threshold_x {3.0f}; // multiple of the estimated noise floor
        size_t margin       {512}; // samples of context kept on each side
        size_t merge_gap     {512}; // merge windows separated by less than this

        // Which quantile of block power to treat as the noise floor.
        //
        // This started as the median, which silently assumes bursts are
        // *sparse* -- fine for an idle channel, but wrong exactly when it
        // matters. Under continuous traffic most blocks contain signal, so
        // the median becomes the signal level, the threshold lands above
        // everything, and detection collapses (measured: decoded frames fell
        // from ~200 to ~12 when the channel went busy). A low quantile still
        // lands in the gaps between frames, so the estimate stays anchored to
        // real noise up to roughly (1 - noise_quantile) channel occupancy.
        float  noise_quantile {0.20f};

        // How fast the retained noise floor may RISE, per batch (0..1).
        //
        // The quantile above is computed within one rxPull batch (~65 ms), so
        // it only holds while that batch contains enough silence. Real traffic
        // is bursty: the transmitter force-flushes a full stage buffer and
        // occupies an entire batch back to back, even when its average airtime
        // is only ~48%. In such a batch every block contains signal, the 20th
        // percentile lands ON the signal, and the threshold rises above the
        // burst -- detection collapses for that whole batch.
        //
        // Measured: node A on air 48% of the time, node B reporting 13%
        // occupancy and 12x fewer bursts (236 vs 2950), while the frames it
        // did find still decoded at 99.4%.
        //
        // The noise floor is a property of the receiver and its environment,
        // not of one 65 ms window, so it is carried across batches: it falls
        // immediately (a genuinely quieter floor is believed at once) and
        // rises only slowly, so a saturated batch cannot drag it up onto the
        // signal. Gain changes still track, just over several batches.
        float  floor_rise {0.05f};
    };

    BurstDetector() = default;
    explicit BurstDetector(const Config& cfg) : cfg_(cfg) {}

    // Returns candidate burst regions in `buf` (empty if nothing crosses the
    // threshold — the common case, since most batches are pure silence/noise).
    // Not const: the noise floor is retained across batches (see
    // Config::floor_rise).
    std::vector<Window> detect(const std::vector<std::complex<float>>& buf);

    // Current retained noise floor, or -1 before the first batch.
    float noiseFloor() const { return noise_floor_; }

private:
    float noise_floor_{-1.0f};   // retained across batches
    Config cfg_;
};

} // namespace sdr
