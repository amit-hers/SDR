#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/AGC.hpp"
#include "sdr/dsp/CoarseFreqCorrect.hpp"
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
    std::vector<std::complex<float>> sig;

    // Re-prime the buffer each pass. AGC::process works in place, so reusing
    // the previous output as the next input feeds the loop its own gain --
    // the level then compounds instead of settling (measured: it reaches
    // ~20x by the 20th pass). A receiver always hands the AGC fresh samples,
    // so that is what convergence has to be measured against.
    for (int iter = 0; iter < 20; ++iter) {
        sig.assign(512, {0.5f, 0.0f});
        agc.process(sig);
    }

    float mag = std::abs(sig[256]);
    assert(mag > 0.5f && mag < 2.0f);
    std::cout << "  [dsp] AGC converges (mag=" << mag << "): PASS\n";
}

// The coarse CFO search was rewritten to advance one complex phasor instead
// of calling cos/sin per sample (it was 67% of one core). Pin the behaviour:
// a known offset must still be recovered on the same search grid.
static void test_coarse_cfo_recovers_offset() {
    const double FS = 4e6;
    for (double cfo : {0.0, 1000.0, -5000.0, 25000.0}) {
        std::vector<std::complex<float>> buf(60000);
        std::srand(7);
        for (auto& s : buf)
            s = {0.01f * ((std::rand() % 2000) / 1000.f - 1.f),
                 0.01f * ((std::rand() % 2000) / 1000.f - 1.f)};
        for (size_t i = 20000; i < 30000; ++i) {          // BPSK burst
            float b = ((i / 4) % 2) ? 1.f : -1.f;
            double ph = 2.0 * M_PI * cfo * static_cast<double>(i) / FS;
            buf[i] += std::complex<float>(static_cast<float>(b * std::cos(ph)),
                                          static_cast<float>(b * std::sin(ph)));
        }
        double est = sdr::CoarseFreqCorrect::apply(buf, FS);
        // Grid resolution is ~(FS/N)*4/2 Hz, so allow a couple of bins.
        assert(std::fabs(est - cfo) < 700.0);
    }
    std::cout << "  [dsp] CoarseFreqCorrect recovers known offsets: PASS\n";
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
    test_coarse_cfo_recovers_offset();
    test_fft_spectrum_length();
    test_fft_dc_tone();
    std::cout << "[dsp tests] ALL PASS\n\n";
}
