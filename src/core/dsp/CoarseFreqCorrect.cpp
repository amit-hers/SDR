#include "sdr/dsp/CoarseFreqCorrect.hpp"
#include <algorithm>
#include <cmath>

namespace sdr {

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

    // ── Bounded correlation search for the residual tone ──────────────────
    // Bounded rather than a full FFT + bin interpretation, to sidestep the
    // doubling fold ambiguity that approach would introduce. The grid is in
    // absolute Hz (see the header) so it does not move with n_used.
    //
    // The tone sits at 2x the CFO, so the grid searched here is doubled too.
    const double step_hz = 2.0 * CFO_STEP_HZ;
    const int    steps   = static_cast<int>(2.0 * MAX_CFO_HZ / step_hz);
    const double nyquist = eff_rate * 0.5;

    double best_mag  = -1.0;
    double best_freq = 0.0;
    for (int k = -steps; k <= steps; ++k) {
        double freq = k * step_hz;
        if (std::fabs(freq) >= nyquist) continue;   // unresolvable after striding
        double dphi = -2.0 * M_PI * freq / eff_rate;

        // Advance the test phasor incrementally rather than evaluating
        // cos/sin at every sample. exp(j*dphi*i) == exp(j*dphi)^i, so one
        // complex multiply per sample replaces two double-precision
        // transcendentals.
        //
        // This search was the single largest CPU consumer in the daemon:
        // measured at 96 ms per burst window, 67% of one core.
        const std::complex<double> rot(std::cos(dphi), std::sin(dphi));
        std::complex<double> p(1.0, 0.0);
        std::complex<double> acc(0.0, 0.0);
        for (size_t i = 0; i < n_used; ++i) {
            acc += sq[i] * p;
            p *= rot;
            // Repeated multiplication lets |p| drift off the unit circle.
            // At double precision the drift over 16k samples is ~1e-12, but
            // renormalising occasionally costs nothing and keeps the result
            // independent of buffer length.
            if ((i & 0x3FF) == 0x3FF) {
                double m = std::abs(p);
                if (m > 0.0) p /= m;
            }
        }
        double mag = std::abs(acc);
        if (mag > best_mag) { best_mag = mag; best_freq = freq; }
    }

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
