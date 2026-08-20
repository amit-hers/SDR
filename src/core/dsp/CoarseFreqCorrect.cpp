#include "sdr/dsp/CoarseFreqCorrect.hpp"
#include <cmath>

namespace sdr {

double CoarseFreqCorrect::apply(std::vector<std::complex<float>>& buf, double sample_rate) {
    if (buf.empty()) return 0.0;

    // A real rxPull batch is mostly silence/noise between sparse, short
    // bursts -- a real frame may occupy only ~1-2% of it. Sample across the
    // WHOLE buffer (strided, not truncated to a prefix) so the estimate
    // isn't blind to a burst sitting outside an arbitrary analysis window.
    constexpr size_t N = 16384;
    const size_t stride = buf.size() > N ? buf.size() / N : 1;
    const double eff_rate = sample_rate / static_cast<double>(stride);

    // Squaring removes the +-1 BPSK data modulation, leaving a residual
    // tone at 2x the carrier offset (see CoarseFreqCorrect.hpp).
    std::vector<std::complex<double>> sq(N, {0.0, 0.0});
    size_t n_used = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t idx = i * stride;
        if (idx >= buf.size()) break;
        std::complex<double> x(buf[idx].real(), buf[idx].imag());
        sq[i] = x * x;
        ++n_used;
    }

    // Bounded correlation search for the residual tone (bounded, not a full
    // FFT+bin-interpretation, to sidestep the doubling fold ambiguity that
    // approach would introduce). Search range/resolution is relative to the
    // effective (post-stride) sample rate.
    double best_mag = -1.0;
    double best_freq = 0.0;
    const int steps = 200;
    const double step_hz = (eff_rate / static_cast<double>(n_used)) * 4.0;
    for (int k = -steps; k <= steps; ++k) {
        double freq = k * step_hz;
        double dphi = -2.0 * M_PI * freq / eff_rate;
        std::complex<double> acc(0.0, 0.0);
        for (size_t i = 0; i < n_used; ++i) {
            double ph = dphi * static_cast<double>(i);
            acc += sq[i] * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        double mag = std::abs(acc);
        if (mag > best_mag) { best_mag = mag; best_freq = freq; }
    }

    double cfo_est = best_freq / 2.0; // squaring doubled the frequency
    double dphi = -2.0 * M_PI * cfo_est / sample_rate;
    double phase = 0.0;
    for (auto& s : buf) {
        std::complex<float> rot(static_cast<float>(std::cos(phase)),
                                static_cast<float>(std::sin(phase)));
        s *= rot;
        phase += dphi;
    }
    return cfo_est;
}

} // namespace sdr
