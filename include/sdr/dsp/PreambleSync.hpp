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

    // Why a search returned what it did.
    //
    // find() reports only the winner, which cannot distinguish "there is no
    // preamble here" from "there is one and the threshold rejected it" --
    // the two call for opposite fixes. These are filled only when a Diag is
    // passed, so the production path is unchanged.
    struct Diag {
        float  peak{0};        // best correlation found (== Match::quality)
        long   peak_off{0};
        float  second{0};      // best peak well away from the winner
        long   second_off{0};  // -1 if no separated peak existed
        size_t searched{0};    // coarse offsets examined
    };

    // Searches `buf` for the preamble. `max_search` bounds how far in to
    // look (0 = whole buffer); the burst detector has already localised
    // things, so a bound keeps the cost down.
    Match find(const std::vector<std::complex<float>>& buf,
               size_t max_search = 0,
               Diag* diag = nullptr) const;

    // ── Failure post-mortem (diagnostic only, NOT on the decode path) ─────
    //
    // Answers the question find() cannot: when a search fails, was the
    // preamble absent, out of range, or present but unmatched?
    //
    // The correlation is phase-invariant -- it takes the magnitude of the
    // accumulated conj(ref)*buf -- but it is NOT frequency-invariant. A
    // residual carrier offset rotates the accumulation across the reference's
    // ~1120 samples and cancels the peak, so a perfectly clean preamble can
    // score at the noise floor. This rescans a WIDER range at several carrier
    // hypotheses; if a strong peak appears at some non-zero df, the preamble
    // was there all along and the failure is carrier, not alignment.
    //
    // Expensive. Call it on a sampled subset of failures only.
    struct Probe {
        bool   ran{false};
        float  best_q{0};      // best over all frequency hypotheses
        long   best_off{0};
        float  best_df{0};     // Hz at which best_q occurred
        float  q_at_df0{0};    // best at zero offset, over the WIDE range
        long   off_at_df0{0};
    };
    Probe probe(const std::vector<std::complex<float>>& buf,
                size_t max_search, double sample_rate,
                double df_max_hz, int df_steps, size_t ref_stride) const;

    // Correlation peak below this is treated as "no preamble here".
    static constexpr float MIN_QUALITY = 0.30f;

    size_t referenceLength() const { return ref_.size(); }

private:
    std::vector<std::complex<float>> ref_;      // RRC-shaped preamble
    double                           ref_energy_{0.0};
};

} // namespace sdr
