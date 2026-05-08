#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <liquid/liquid.h>

namespace sdr {

class FFTSpectrum {
public:
    static constexpr int BINS = 256;

    FFTSpectrum();
    ~FFTSpectrum();
    FFTSpectrum(const FFTSpectrum&)            = delete;
    FFTSpectrum& operator=(const FFTSpectrum&) = delete;

    // Push one sample; FFT fires automatically every BINS samples.
    void push(std::complex<float> sample);
    // Batch push.
    void accumulate(const std::complex<float>* samples, int n);

    // True when a new spectrum is available since last get().
    bool ready() const;
    // Retrieve spectrum and clear ready flag.
    const std::array<float, BINS>& get();

    // Legacy: always-valid spectrum (last computed).
    const std::array<float, BINS>& spectrum() const { return power_db_; }

    double center_hz   {434e6};
    double sample_rate {20e6};

private:
    fftplan                            plan_    {nullptr};
    std::array<liquid_float_complex, BINS> buf_ {};
    std::array<liquid_float_complex, BINS> fft_ {};
    std::array<float, BINS>            power_db_{};
    int                                fill_   {0};
    bool                               ready_  {false};
};

// Alias used by stats
static constexpr int FFT_BINS = FFTSpectrum::BINS;

} // namespace sdr
