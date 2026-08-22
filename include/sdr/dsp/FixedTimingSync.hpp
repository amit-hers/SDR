#pragma once
#include <complex>
#include <cstdint>
#include <vector>

namespace sdr {

// Fixed-point, hardware-realizable equivalent of TimingSync: RRC matched
// filter + timing recovery + 4:1 decimation to one sample per symbol.
//
// This is the golden model for an FPGA implementation, not an optimisation
// of the software path. Everything here maps directly onto fabric:
//
//   * coefficients and data are Q1.15, products accumulate in 40 bits
//   * the matched filter is a 32-tap transposed-form FIR (32 DSP48s, or 16
//     with coefficient symmetry)
//   * fractional delay uses a cubic Farrow structure -- 4 fixed sub-filters
//     and 3 multiplies, no per-phase coefficient memory
//   * the timing error detector is Gardner: one multiply, no decisions, and
//     it needs only 2 samples/symbol so it runs at half the input rate
//   * the loop filter is proportional-integral with shift-based gains
//
// Deliberately NOT a port of liquid's symsync_crcf: that uses a polyphase
// bank with a per-phase coefficient set, which costs BRAM and a variable
// filter index. Farrow trades that for a fixed datapath, which is the right
// shape for a streaming block.
//
// The point of having this in C++ is to answer -- on recorded captures,
// before any HDL exists -- whether fixed-point arithmetic and a different
// interpolator still deliver the same symbols and the same CRCs.
class FixedTimingSync {
public:
    struct Config {
        int    sps            {4};      // input samples per symbol
        double loop_bw        {0.02};   // normalised timing loop bandwidth
        int    coeff_bits     {16};     // Q1.15
        int    acc_bits       {40};     // FIR accumulator width
        // Constant offset, in symbol periods, between where the timing loop
        // locks and where the output symbol is taken. Gardner drives to its
        // own equilibrium (the zero crossing of the TED), and that point is
        // not necessarily the ISI-optimal sampling instant -- it depends on
        // the matched filter's group delay and the half-symbol convention in
        // the error term. Offsetting the output rather than the loop lets the
        // sampling phase be corrected without disturbing tracking.
        double phase_offset   {0.0};

        // Interpolator structure.
        //
        // FARROW: cubic Lagrange on the matched-filter output. Fixed
        //   datapath, 3 multiplies, no coefficient memory -- cheapest in
        //   fabric, but Lagrange has real passband droop.
        // POLYPHASE: a bank of fractionally-delayed RRC filters. The bank IS
        //   the matched filter, so there is no separate FIR stage before it;
        //   costs BRAM for the coefficient bank but has the designed response.
        enum class Interp { FARROW, POLYPHASE };
        Interp interp     {Interp::FARROW};
        int    n_phases   {16};      // POLYPHASE only

        // Run the timing loop in integer arithmetic (Q16.16 NCO, shift-based
        // error normalisation) instead of double. Required for a bit-exact
        // comparison against HDL: as long as the loop uses floating point,
        // the reference cannot be reproduced by fabric.
        bool   fixed_loop {false};

        // Loop gains as right-shift counts, used when fixed_loop is set.
        // Derived analytically these came out at 15/22, which left CRC 12
        // points short of the double-precision loop -- the analytic mapping
        // ignores the error clamp and the quantisation of err itself, so the
        // effective gains are not what the algebra predicts. Swept instead.
        // Swept on recorded captures, not derived: the analytic mapping gave
        // 15/22 and scored 12 points below the double-precision loop. CRC
        // falls off sharply with larger alpha_sh (17 -> 74%) and is almost
        // flat in beta_sh, so the proportional gain is what matters.
        int    alpha_sh   {12};
        int    beta_sh    {22};
    };

    struct Result {
        std::vector<std::complex<float>> syms;   // one per symbol
        std::vector<double>              mu;     // fractional delay used, per symbol
        std::vector<int>                 idx;    // input index each symbol came from
        std::vector<int>                 phase;  // polyphase index used
        std::vector<int>                 hidx;   // half-symbol input index
        std::vector<int>                 hphase; // half-symbol polyphase index
        std::vector<double>              raw_e;  // Gardner product, pre-normalisation
        std::vector<double>              pwr;    // power used to normalise it
        std::vector<long long>           e_q;    // normalised error, Q(EQ)
        uint64_t                         sat_count{0};  // accumulator saturations
    };

    explicit FixedTimingSync(const Config& cfg);

    // Streams `in` through matched filter -> timing recovery -> decimation.
    Result process(const std::vector<std::complex<float>>& in);

    // Quantised matched-filter taps, exposed so the HDL coefficient ROM can
    // be generated from exactly what this model used.
    const std::vector<int16_t>& taps() const { return taps_q15_; }

    // Flattened polyphase bank: phase p, tap k at bank_q15_[p*taps_per_phase_+k].
    // This is what an HDL coefficient ROM would be initialised from.
    const std::vector<int16_t>& bank() const { return bank_q15_; }
    int tapsPerPhase() const { return taps_per_phase_; }

private:
    Config               cfg_;
    std::vector<int16_t> taps_q15_;
    std::vector<int16_t> bank_q15_;
    int                  taps_per_phase_{0};
};

} // namespace sdr
