#include "sdr/dsp/BurstDetector.hpp"
#include <algorithm>

namespace sdr {

std::vector<BurstDetector::Window>
BurstDetector::detect(const std::vector<std::complex<float>>& buf) const {
    std::vector<Window> windows;
    const size_t block = cfg_.block_size;
    if (buf.size() < block * 4) return windows; // too short to bother

    size_t n_blocks = buf.size() / block;
    std::vector<float> power(n_blocks);
    for (size_t b = 0; b < n_blocks; ++b) {
        double acc = 0.0;
        for (size_t i = 0; i < block; ++i) {
            const auto& s = buf[b * block + i];
            acc += static_cast<double>(s.real()) * s.real()
                 + static_cast<double>(s.imag()) * s.imag();
        }
        power[b] = static_cast<float>(acc / static_cast<double>(block));
    }

    std::vector<float> sorted_power = power;
    std::sort(sorted_power.begin(), sorted_power.end());
    float noise_floor = sorted_power[sorted_power.size() / 2]; // median
    float threshold   = noise_floor * cfg_.threshold_x;

    // Find contiguous (block-granularity) elevated regions.
    std::vector<Window> raw;
    bool in_region = false;
    size_t region_start = 0;
    for (size_t b = 0; b < n_blocks; ++b) {
        bool above = power[b] > threshold;
        if (above && !in_region) { in_region = true; region_start = b; }
        if (!above && in_region) {
            in_region = false;
            raw.push_back({region_start * block, b * block});
        }
    }
    if (in_region) raw.push_back({region_start * block, n_blocks * block});
    if (raw.empty()) return windows;

    // Merge regions separated by less than merge_gap, then apply margin.
    for (const auto& w : raw) {
        if (!windows.empty() && w.start <= windows.back().end + cfg_.merge_gap) {
            windows.back().end = w.end;
        } else {
            windows.push_back(w);
        }
    }
    for (auto& w : windows) {
        w.start = (w.start > cfg_.margin) ? w.start - cfg_.margin : 0;
        w.end   = std::min(buf.size(), w.end + cfg_.margin);
    }
    return windows;
}

} // namespace sdr
