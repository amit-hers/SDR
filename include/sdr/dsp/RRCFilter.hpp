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
