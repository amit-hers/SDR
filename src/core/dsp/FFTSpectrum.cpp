#include "sdr/dsp/FFTSpectrum.hpp"
#include <cmath>
#include <cstring>

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

FFTSpectrum::FFTSpectrum() {
    plan_ = fft_create_plan(static_cast<unsigned>(BINS),
                            buf_.data(), fft_.data(),
                            LIQUID_FFT_FORWARD, 0);
}

FFTSpectrum::~FFTSpectrum() {
    if (plan_) fft_destroy_plan(plan_);
}

void FFTSpectrum::push(std::complex<float> sample) {
    buf_[static_cast<size_t>(fill_)] = to_lc(sample);
    if (++fill_ == BINS) {
        fill_ = 0;
        fft_execute(plan_);
        for (int b = 0; b < BINS; ++b) {
            int sb = (b + BINS / 2) % BINS;
            auto fc = from_lc(fft_[static_cast<size_t>(sb)]);
            float mag2 = fc.real() * fc.real() + fc.imag() * fc.imag();
            power_db_[static_cast<size_t>(b)] =
                10.f * std::log10(mag2 / static_cast<float>(BINS) + 1e-12f);
        }
        ready_ = true;
    }
}

void FFTSpectrum::accumulate(const std::complex<float>* samples, int n) {
    for (int i = 0; i < n; ++i) push(samples[i]);
}

bool FFTSpectrum::ready() const { return ready_; }

const std::array<float, FFTSpectrum::BINS>& FFTSpectrum::get() {
    ready_ = false;
    return power_db_;
}

} // namespace sdr
