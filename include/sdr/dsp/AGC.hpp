#pragma once
#include <complex>
#include <vector>
#include <liquid/liquid.h>

namespace sdr {

class AGC {
public:
    AGC();
    ~AGC();
    AGC(const AGC&)            = delete;
    AGC& operator=(const AGC&) = delete;

    std::complex<float> process(std::complex<float> in);
    void                process(std::vector<std::complex<float>>& buf); // in-place batch
    float               rssi_dbm() const;  // estimated RSSI in dBm
    void                reset();

private:
    agc_crcf agc_{nullptr};
};

} // namespace sdr
