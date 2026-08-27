#pragma once
#include <complex>
#include <vector>
#include <liquid/liquid.h>

namespace sdr {

static constexpr int   RRC_SPS     = 4;
static constexpr float RRC_ROLLOFF = 0.35f;

// Fraction of the symbol rate the shaped signal actually occupies, with a
// little margin above the theoretical (1 + rolloff). The radio's analog
// filter must be set from this, not from the symbol rate: filtering at the
// symbol rate removes the pulse skirts that the matched filter needs.
static constexpr double RRC_OCCUPIED_BW_FACTOR = 1.4;
static constexpr int   RRC_TAPS    = 32;

// Symbols of pulse span on EACH side of centre -- liquid's `m` parameter.
// Total prototype length is 2*sps*m + 1.
//
// This is deliberately separate from RRC_TAPS, which this codebase uses with
// two different meanings: RRCInterp/RRCDecim/TimingSync pass it to liquid as
// `m` (so it meant a 64-symbol span, 257 taps), while FixedTimingSync computes
// `RRC_TAPS / sps` and treats it as a tap count (an 8-symbol span). The
// receive path has therefore always matched-filtered with span 8 against a
// span-64 transmit pulse, and still recovered 95% of frames -- the extra
// transmit span was doing nothing.
//
// 6 is chosen so the matched filter is implementable in fabric: 2*4*6+1 = 49
// taps, against 257 for m=32. A fully unrolled complex FIR at II=1 costs two
// multiplies per tap, so 257 taps needs 514 DSP48 slices and a Zynq-7020 has
// 220. 49 taps needs 98 and fits.
static constexpr int   RRC_SPAN_SYMS = 6;

class RRCInterp {
public:
    explicit RRCInterp(int sps = RRC_SPS);
    ~RRCInterp();
    RRCInterp(const RRCInterp&)            = delete;
    RRCInterp& operator=(const RRCInterp&) = delete;

    // Single-symbol → SPS samples appended to out.
    void push(std::complex<float> sym, std::vector<std::complex<float>>& out);
    // Batch: all symbols → upsampled output.
    void process(const std::vector<std::complex<float>>& in,
                 std::vector<std::complex<float>>& out);
    void reset();

private:
    firinterp_crcf interp_{nullptr};
    int            sps_;
};

class RRCDecim {
public:
    explicit RRCDecim(int sps = RRC_SPS);
    ~RRCDecim();
    RRCDecim(const RRCDecim&)            = delete;
    RRCDecim& operator=(const RRCDecim&) = delete;

    // Single sample → one decimated symbol (returns true when ready).
    bool push(std::complex<float> in, std::complex<float>& out);
    // Batch: accumulated input → all decimated symbols.
    void process(const std::vector<std::complex<float>>& in,
                 std::vector<std::complex<float>>& out);
    void reset();

private:
    firdecim_crcf                   decim_{nullptr};
    int                             sps_;
    int                             count_{0};
    std::vector<liquid_float_complex> buf_;
};

} // namespace sdr
