#pragma once
#include <complex>
#include <vector>
#include <cstddef>

namespace sdr {

// Data-aided carrier recovery: estimates carrier phase (and residual
// frequency) by comparing received symbols against the preamble+sync word,
// whose bits the receiver already knows.
//
// This exists because a Costas loop performs badly on these bursts, for a
// structural reason. The preamble is PREAMBLE_BYTE (0xAA) -- a perfectly
// alternating bit pattern -- which in BPSK is a *suppressed carrier*: all
// its energy sits at +/- half the symbol rate and none at the carrier. The
// loop therefore has almost nothing to pull in on, and by the time real
// payload data arrives to drive it the sync word has already gone past.
// Measured on captured hardware bursts: after Costas, mean|I| ~= mean|Q|,
// i.e. the constellation was still rotating rather than collapsing onto the
// real axis, which sprayed bit errors through the frame.
//
// Estimating directly from known symbols is deterministic -- no convergence
// time, which suits short bursts -- and it recovers *absolute* phase, so it
// also resolves the BPSK 180-degree ambiguity outright instead of guessing
// polarity after the fact.
class DataAidedSync {
public:
    DataAidedSync();

    struct Result {
        bool   ok            {false};
        int    sym_offset    {0};     // where the reference starts, in symbols
        float  phase         {0.f};   // radians
        float  freq_per_sym  {0.f};   // radians/symbol (residual CFO)
        float  quality       {0.f};   // normalised correlation, 0..1
    };

    // Searches the first `max_sym_search` symbol positions for the reference
    // sequence and estimates phase/frequency there.
    Result estimate(const std::vector<std::complex<float>>& syms,
                    int max_sym_search = 64) const;

    // Removes the estimated phase ramp in place (open loop).
    static void derotate(std::vector<std::complex<float>>& syms, const Result& r);

    // Removes the phase ramp while continuously re-estimating it from the
    // decided symbols (decision-directed tracking).
    //
    // The open-loop version above measures phase over the preamble+sync and
    // then *extrapolates* across the rest of the frame. Any residual
    // frequency error accumulates: measured on hardware, ~0.0009 rad/symbol
    // over a ~2100-symbol frame is ~109 degrees, so phase crosses the
    // 90-degree BPSK decision boundary about 83% of the way in and every
    // remaining bit inverts. That is ~45 consecutive corrupted bytes -- far
    // past what RS(255,223) can repair -- which is why such frames parse a
    // perfect header and still fail CRC. Tracking keeps the estimate honest
    // through the payload instead of trusting one extrapolation.
    static void derotateTracked(std::vector<std::complex<float>>& syms,
                                const Result& r,
                                float alpha = 0.02f);

    static constexpr float MIN_QUALITY = 0.45f;
    size_t referenceSymbols() const { return ref_.size(); }

private:
    std::vector<std::complex<float>> ref_;   // +1/-1 BPSK symbols
};

} // namespace sdr
