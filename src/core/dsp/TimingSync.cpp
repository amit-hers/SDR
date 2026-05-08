#include "sdr/dsp/TimingSync.hpp"
#include <cstring>
#include <stdexcept>

namespace sdr {

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

TimingSync::TimingSync(int sps) : sps_(sps) {
    sync_ = symsync_crcf_create_rnyquist(
        LIQUID_FIRFILT_RRC, static_cast<unsigned>(sps_),
        RRC_TAPS, RRC_ROLLOFF, 32);
    if (!sync_)
        throw std::runtime_error("TimingSync: symsync_crcf_create failed");
    symsync_crcf_set_lf_bw(sync_, 0.02f);
}

TimingSync::~TimingSync() {
    if (sync_) symsync_crcf_destroy(sync_);
}

void TimingSync::push(std::complex<float> in,
                      std::vector<std::complex<float>>& out) {
    liquid_float_complex lin = to_lc(in);
    liquid_float_complex lout[8]{};
    unsigned n_out = 0;
    symsync_crcf_execute(sync_, &lin, 1, lout, &n_out);
    for (unsigned i = 0; i < n_out; ++i)
        out.push_back(from_lc(lout[i]));
}

void TimingSync::process(const std::vector<std::complex<float>>& in,
                          std::vector<std::complex<float>>& out) {
    out.clear();
    out.reserve(in.size() / static_cast<size_t>(sps_) + 4);
    for (auto& s : in) push(s, out);
}

void TimingSync::reset() {
    symsync_crcf_reset(sync_);
}

} // namespace sdr
