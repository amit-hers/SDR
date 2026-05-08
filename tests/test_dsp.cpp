#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/AGC.hpp"
#include "sdr/dsp/TimingSync.hpp"
#include "sdr/dsp/CostasLoop.hpp"
#include "sdr/dsp/FFTSpectrum.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <complex>
#include <algorithm>
#include <cmath>
#include <numeric>

static void test_rrc_shape() {
    sdr::RRCInterp interp;
    sdr::RRCDecim  decim;

    // QPSK symbols
    std::vector<std::complex<float>> syms = {
        {.707f,.707f},{-.707f,.707f},{-.707f,-.707f},{.707f,-.707f},
        {.707f,.707f},{.707f,-.707f}
    };

    std::vector<std::complex<float>> up, down;
    interp.process(syms, up);
    assert(up.size() == syms.size() * static_cast<size_t>(sdr::RRC_SPS));

    decim.process(up, down);
    assert(!down.empty());
    std::cout << "  [dsp] RRC interp/decim size OK: PASS\n";
}

static void test_agc_settles() {
    sdr::AGC agc;
    std::vector<std::complex<float>> sig(512, {0.5f, 0.0f});
    for (int iter = 0; iter < 20; ++iter)
        agc.process(sig);

    float mag = std::abs(sig[256]);
    assert(mag > 0.5f && mag < 2.0f);
    std::cout << "  [dsp] AGC converges (mag=" << mag << "): PASS\n";
}

static void test_fft_spectrum_length() {
    sdr::FFTSpectrum fft;
    for (int i = 0; i < sdr::FFTSpectrum::BINS; ++i)
        fft.push({std::cos(static_cast<float>(i) * 0.1f),
                  std::sin(static_cast<float>(i) * 0.1f)});

    assert(fft.ready());
    auto& spec = fft.get();
    assert(static_cast<int>(spec.size()) == sdr::FFTSpectrum::BINS);
    for (float s : spec) { (void)s; assert(std::isfinite(s)); }
    std::cout << "  [dsp] FFT spectrum length + finite values: PASS\n";
}

static void test_fft_dc_tone() {
    sdr::FFTSpectrum fft;
    // Feed extra samples to trigger a second window (avoid leftover from previous test)
    for (int i = 0; i < sdr::FFTSpectrum::BINS * 2; ++i)
        fft.push({1.0f, 0.0f});

    assert(fft.ready());
    auto& spec = fft.get();
    int peak = static_cast<int>(
        std::max_element(spec.begin(), spec.end()) - spec.begin());
    assert(std::abs(peak - sdr::FFTSpectrum::BINS / 2) <= 2);
    std::cout << "  [dsp] FFT DC peak at center bin " << peak << ": PASS\n";
}

void run_dsp() {
    std::cout << "[dsp tests]\n";
    test_rrc_shape();
    test_agc_settles();
    test_fft_spectrum_length();
    test_fft_dc_tone();
    std::cout << "[dsp tests] ALL PASS\n\n";
}
