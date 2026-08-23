#include "sdr/dsp/CoarseFreqCorrect.hpp"
#include <algorithm>
#include <cmath>
#include <liquid/liquid.h>
#include <cstring>

namespace sdr {

CoarseFreqCorrect::Method CoarseFreqCorrect::method_ = CoarseFreqCorrect::Method::GRID;

// Bounded correlation search over a fixed absolute grid. Cost is
// grid_points * n_used complex multiplies.
double CoarseFreqCorrect::estimateGrid(const std::vector<std::complex<double>>& sq,
                                       double eff_rate) {
    const size_t n_used  = sq.size();
    const double step_hz = 2.0 * CFO_STEP_HZ;
    const int    steps   = static_cast<int>(2.0 * MAX_CFO_HZ / step_hz);
    const double nyquist = eff_rate * 0.5;

    double best_mag = -1.0, best_freq = 0.0;
    for (int k = -steps; k <= steps; ++k) {
        double freq = k * step_hz;
        if (std::fabs(freq) >= nyquist) continue;
        double dphi = -2.0 * M_PI * freq / eff_rate;
        const std::complex<double> rot(std::cos(dphi), std::sin(dphi));
        std::complex<double> p(1.0, 0.0), acc(0.0, 0.0);
        for (size_t i = 0; i < n_used; ++i) {
            acc += sq[i] * p;
            p *= rot;
            if ((i & 0x3FF) == 0x3FF) { double m = std::abs(p); if (m > 0.0) p /= m; }
        }
        double mag = std::abs(acc);
        if (mag > best_mag) { best_mag = mag; best_freq = freq; }
    }
    return best_freq;
}

// One FFT of the squared signal; the residual tone is the largest bin inside
// the search range. Parabolic interpolation on the peak recovers sub-bin
// resolution, which the raw bin spacing (eff_rate/N) would otherwise lose --
// at 4 MSPS and N=4096 a bin is 977 Hz, versus the grid's 500 Hz step.
double CoarseFreqCorrect::estimateFft(const std::vector<std::complex<double>>& sq,
                                      double eff_rate) {
    size_t N = 1;
    while (N < sq.size()) N <<= 1;          // radix-2 for liquid's planner

    std::vector<liquid_float_complex> in(N), out(N);
    std::fill(in.begin(), in.end(), liquid_float_complex{});
    for (size_t i = 0; i < sq.size(); ++i) {
        std::complex<float> v(static_cast<float>(sq[i].real()),
                              static_cast<float>(sq[i].imag()));
        std::memcpy(&in[i], &v, sizeof(v));
    }

    fftplan plan = fft_create_plan(static_cast<unsigned>(N), in.data(), out.data(),
                                   LIQUID_FFT_FORWARD, 0);
    fft_execute(plan);
    fft_destroy_plan(plan);

    // Only bins within +/-2*MAX_CFO_HZ are candidates (the tone sits at twice
    // the offset). Anything outside is out of the estimator's declared range.
    const double bin_hz  = eff_rate / static_cast<double>(N);
    const long   max_bin = static_cast<long>(2.0 * MAX_CFO_HZ / bin_hz);

    auto mag = [&](long k) {
        size_t idx = static_cast<size_t>(((k % static_cast<long>(N)) + static_cast<long>(N))
                                         % static_cast<long>(N));
        std::complex<float> v;
        std::memcpy(&v, &out[idx], sizeof(v));
        return static_cast<double>(std::abs(v));
    };

    long best_k = 0; double best = -1.0;
    for (long k = -max_bin; k <= max_bin; ++k) {
        double m = mag(k);
        if (m > best) { best = m; best_k = k; }
    }

    // Parabolic peak interpolation across the three bins around the maximum.
    double ym1 = mag(best_k - 1), y0 = mag(best_k), yp1 = mag(best_k + 1);
    double denom = (ym1 - 2.0 * y0 + yp1);
    double delta = (std::fabs(denom) > 1e-12) ? 0.5 * (ym1 - yp1) / denom : 0.0;
    if (delta > 0.5) delta = 0.5;
    if (delta < -0.5) delta = -0.5;

    return (static_cast<double>(best_k) + delta) * bin_hz;
}


double CoarseFreqCorrect::apply(std::vector<std::complex<float>>& buf, double sample_rate) {
    // No burst region to aim at: scan the whole buffer, strided. A prefix
    // here would be blind to a burst sitting late in the batch.
    return applyImpl(buf, sample_rate, 0, buf.size(), MAX_SCAN_SAMPLES, /*strided=*/true);
}

double CoarseFreqCorrect::apply(std::vector<std::complex<float>>& buf, double sample_rate,
                                size_t est_begin, size_t est_end) {
    // A burst region *is* the signal, so take a contiguous prefix of it and
    // stop. Striding would decimate, which drops the effective sample rate
    // and with it the resolvable offset range -- the opposite of what a
    // cheap estimate should trade away.
    return applyImpl(buf, sample_rate, est_begin, est_end, MAX_EST_SAMPLES, /*strided=*/false);
}

double CoarseFreqCorrect::applyImpl(std::vector<std::complex<float>>& buf, double sample_rate,
                                    size_t est_begin, size_t est_end,
                                    size_t max_samples, bool strided) {
    if (buf.empty()) return 0.0;

    // ── Choose the analysis region ────────────────────────────────────────
    est_begin = std::min(est_begin, buf.size());
    est_end   = std::min(est_end,   buf.size());
    if (est_end <= est_begin) { est_begin = 0; est_end = buf.size(); }
    const size_t span = est_end - est_begin;

    const size_t stride = (strided && span > max_samples) ? span / max_samples : 1;
    const double eff_rate = sample_rate / static_cast<double>(stride);

    // Squaring removes the +-1 BPSK data modulation, leaving a residual
    // tone at 2x the carrier offset (see CoarseFreqCorrect.hpp).
    std::vector<std::complex<double>> sq;
    sq.reserve(std::min(span, max_samples));
    for (size_t i = 0; i < max_samples; ++i) {
        size_t idx = est_begin + i * stride;
        if (idx >= est_end) break;
        std::complex<double> x(buf[idx].real(), buf[idx].imag());
        sq.push_back(x * x);
    }
    const size_t n_used = sq.size();
    if (n_used < 8) return 0.0;

    // ── Estimate the residual tone ────────────────────────────────────────
    const double best_freq = (method_ == Method::FFT)
                           ? estimateFft(sq, eff_rate)
                           : estimateGrid(sq, eff_rate);

    // ── Correct the WHOLE buffer ──────────────────────────────────────────
    // Not just the analysis region: everything here is demodulated later.
    const double cfo_est = best_freq / 2.0;   // squaring doubled the frequency
    const double dphi    = -2.0 * M_PI * cfo_est / sample_rate;
    const std::complex<double> rot(std::cos(dphi), std::sin(dphi));
    std::complex<double> p(1.0, 0.0);
    size_t i = 0;
    for (auto& s : buf) {
        s *= std::complex<float>(static_cast<float>(p.real()),
                                 static_cast<float>(p.imag()));
        p *= rot;
        if ((++i & 0x3FF) == 0) {
            double m = std::abs(p);
            if (m > 0.0) p /= m;
        }
    }
    return cfo_est;
}

} // namespace sdr
