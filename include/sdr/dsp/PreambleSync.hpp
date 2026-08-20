#pragma once
#include <complex>
#include <vector>
#include <cstddef>

namespace sdr {

// Locates the start of a frame inside a burst by correlating against the
// known preamble waveform.
//
// This replaces brute-force alignment search. TimingSync cannot reliably
// acquire symbol timing from an arbitrary start offset -- whether it locks
// depends brittlely on where the window begins relative to the RRC tap grid
// -- so the receiver previously retried the whole AGC/timing/carrier/demod
// chain at all RRC_TAPS alignments and took whichever worked. That is
// correct but costs 32x a full chain per burst, which saturates the CPU at
// realistic frame rates and starves reception.
//
// Correlating against the preamble *computes* the alignment instead of
// searching for it: one cheap correlation, then one demod pass at the
// offset it found. The frame is then presented to the chain starting
// exactly at the preamble, which is the alignment that works reliably.
//
// BPSK carries an unknown phase (and the preamble is its own bit-inverse
// under a 180 degree flip), so correlation uses complex magnitude and is
// insensitive to carrier phase.
class PreambleSync {
public:
    explicit PreambleSync(int sps);

    struct Match {
        long   offset;      // sample index where the preamble starts
        float  quality;     // normalised correlation peak, 0..1-ish
        bool   found;
    };

    // Searches `buf` for the preamble. `max_search` bounds how far in to
    // look (0 = whole buffer); the burst detector has already localised
    // things, so a bound keeps the cost down.
    Match find(const std::vector<std::complex<float>>& buf,
               size_t max_search = 0) const;

    // Correlation peak below this is treated as "no preamble here".
    static constexpr float MIN_QUALITY = 0.30f;

    size_t referenceLength() const { return ref_.size(); }

private:
    std::vector<std::complex<float>> ref_;      // RRC-shaped preamble
    double                           ref_energy_{0.0};
};

} // namespace sdr
