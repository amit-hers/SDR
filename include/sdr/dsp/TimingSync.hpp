#pragma once
#include <complex>
#include <vector>
#include <liquid/liquid.h>
#include "sdr/dsp/RRCFilter.hpp"   // for RRC_SPS, RRC_TAPS, RRC_ROLLOFF

namespace sdr {

class TimingSync {
public:
    explicit TimingSync(int sps = RRC_SPS);
    ~TimingSync();
    TimingSync(const TimingSync&)            = delete;
    TimingSync& operator=(const TimingSync&) = delete;

    // Feed one sample; appends synchronised symbols to out.
    void push(std::complex<float> in, std::vector<std::complex<float>>& out);
    // Batch version.
    void process(const std::vector<std::complex<float>>& in,
                 std::vector<std::complex<float>>& out);
    void reset();

private:
    symsync_crcf sync_{nullptr};
    int          sps_;
};

} // namespace sdr
