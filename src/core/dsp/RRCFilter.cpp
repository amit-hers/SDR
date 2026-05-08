#include "sdr/dsp/RRCFilter.hpp"
#include <cstring>
#include <stdexcept>

namespace sdr {

// liquid_float_complex ↔ std::complex<float> — same memory layout per C++11
static inline liquid_float_complex to_lc(std::complex<float> c) {
    liquid_float_complex lc;
    std::memcpy(&lc, &c, sizeof(lc));
    return lc;
}
static inline std::complex<float> from_lc(liquid_float_complex lc) {
    std::complex<float> c;
    std::memcpy(&c, &lc, sizeof(c));
    return c;
}

// ── RRCInterp ─────────────────────────────────────────────────────────────

RRCInterp::RRCInterp(int sps) : sps_(sps) {
    interp_ = firinterp_crcf_create_prototype(
        LIQUID_FIRFILT_RRC, static_cast<unsigned>(sps_),
        RRC_TAPS, RRC_ROLLOFF, 0.f);
    if (!interp_)
        throw std::runtime_error("RRCInterp: firinterp_crcf_create_prototype failed");
}

RRCInterp::~RRCInterp() {
    if (interp_) firinterp_crcf_destroy(interp_);
}

void RRCInterp::push(std::complex<float> sym,
                     std::vector<std::complex<float>>& out) {
    auto in_lc = to_lc(sym);
    // firinterp_crcf_execute writes sps_ output samples
    std::vector<liquid_float_complex> buf(static_cast<size_t>(sps_));
    firinterp_crcf_execute(interp_, in_lc, buf.data());
    for (int i = 0; i < sps_; ++i)
        out.push_back(from_lc(buf[static_cast<size_t>(i)]));
}

void RRCInterp::process(const std::vector<std::complex<float>>& in,
                         std::vector<std::complex<float>>& out) {
    out.clear();
    out.reserve(in.size() * static_cast<size_t>(sps_));
    for (auto& s : in) push(s, out);
}

void RRCInterp::reset() {
    firinterp_crcf_reset(interp_);
}

// ── RRCDecim ──────────────────────────────────────────────────────────────

RRCDecim::RRCDecim(int sps) : sps_(sps), buf_(static_cast<size_t>(sps)) {
    decim_ = firdecim_crcf_create_prototype(
        LIQUID_FIRFILT_RRC, static_cast<unsigned>(sps_),
        RRC_TAPS, RRC_ROLLOFF, 0.f);
    if (!decim_)
        throw std::runtime_error("RRCDecim: firdecim_crcf_create_prototype failed");
}

RRCDecim::~RRCDecim() {
    if (decim_) firdecim_crcf_destroy(decim_);
}

bool RRCDecim::push(std::complex<float> in, std::complex<float>& out) {
    buf_[static_cast<size_t>(count_++)] = to_lc(in);
    if (count_ >= sps_) {
        count_ = 0;
        liquid_float_complex lout{};
        firdecim_crcf_execute(decim_, buf_.data(), &lout);
        out = from_lc(lout);
        return true;
    }
    return false;
}

void RRCDecim::process(const std::vector<std::complex<float>>& in,
                        std::vector<std::complex<float>>& out) {
    out.clear();
    out.reserve(in.size() / static_cast<size_t>(sps_) + 1);
    std::complex<float> sym;
    for (auto& s : in)
        if (push(s, sym)) out.push_back(sym);
}

void RRCDecim::reset() {
    firdecim_crcf_reset(decim_);
    count_ = 0;
}

} // namespace sdr
