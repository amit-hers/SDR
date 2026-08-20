#include "sdr/dsp/DataAidedSync.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/modem/Modem.hpp"
#include <cmath>
#include <algorithm>

namespace sdr {

DataAidedSync::DataAidedSync() {
    // Reference = preamble + sync word, as BPSK symbols (no pulse shaping --
    // this runs on symbol-rate output from TimingSync, not on samples).
    std::vector<uint8_t> bytes(PREAMBLE_LEN, PREAMBLE_BYTE);
    bytes.push_back(static_cast<uint8_t>((FRAME_SYNC >> 24) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((FRAME_SYNC >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((FRAME_SYNC >>  8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>( FRAME_SYNC        & 0xFF));

    Modem modem(ModScheme::BPSK);
    modem.modulate(bytes.data(), static_cast<int>(bytes.size()), ref_);
}

DataAidedSync::Result
DataAidedSync::estimate(const std::vector<std::complex<float>>& syms,
                        int max_sym_search) const {
    Result best;
    const size_t L = ref_.size();
    if (syms.size() <= L) return best;

    size_t limit = syms.size() - L;
    if (max_sym_search > 0 && static_cast<size_t>(max_sym_search) < limit)
        limit = static_cast<size_t>(max_sym_search);

    double ref_e = 0.0;
    for (const auto& r : ref_) ref_e += std::norm(r);
    ref_e = std::sqrt(ref_e);
    if (ref_e <= 0.0) return best;

    for (size_t d = 0; d <= limit; ++d) {
        // Correlating against the known symbols leaves the carrier rotation
        // as the argument of the accumulator.
        std::complex<double> acc(0.0, 0.0);
        double sig_e = 0.0;
        for (size_t k = 0; k < L; ++k) {
            const auto& x = syms[d + k];
            const auto& r = ref_[k];
            acc += std::complex<double>(x.real(), x.imag()) *
                   std::conj(std::complex<double>(r.real(), r.imag()));
            sig_e += std::norm(x);
        }
        double denom = std::sqrt(sig_e) * ref_e;
        float q = (denom > 0.0) ? static_cast<float>(std::abs(acc) / denom) : 0.f;
        if (q > best.quality) {
            best.quality    = q;
            best.sym_offset = static_cast<int>(d);
            best.phase      = static_cast<float>(std::arg(acc));
        }
    }

    if (best.quality < MIN_QUALITY) return best;

    // Residual frequency: fit the phase ramp across the reference.
    //
    // A two-point estimate (first half vs second half) is too crude -- with
    // realistic symbol noise it under-estimated the drift by ~4x on captured
    // hardware bursts, which left enough uncorrected rotation to carry the
    // constellation past the BPSK decision boundary partway through the
    // payload. Correlating in several short blocks and least-squares fitting
    // the unwrapped phases averages that noise down and gives a slope that
    // actually holds across the whole frame.
    constexpr size_t NBLK = 8;
    const size_t blk = L / NBLK;
    if (blk >= 8) {
        std::vector<double> ph(NBLK), xs(NBLK);
        double prev = 0.0, unwrapped = 0.0;
        size_t used = 0;
        for (size_t b = 0; b < NBLK; ++b) {
            std::complex<double> acc(0.0, 0.0);
            for (size_t k = b * blk; k < (b + 1) * blk; ++k) {
                const auto& x = syms[static_cast<size_t>(best.sym_offset) + k];
                const auto& r = ref_[k];
                acc += std::complex<double>(x.real(), x.imag()) *
                       std::conj(std::complex<double>(r.real(), r.imag()));
            }
            if (std::abs(acc) <= 0.0) continue;
            double p = std::arg(acc);
            if (used == 0) { unwrapped = p; }
            else {
                double d = p - prev;
                while (d >  M_PI) d -= 2.0 * M_PI;   // keep the ramp continuous
                while (d < -M_PI) d += 2.0 * M_PI;
                unwrapped += d;
            }
            prev = p;
            xs[used] = static_cast<double>(b * blk) + 0.5 * static_cast<double>(blk);
            ph[used] = unwrapped;
            ++used;
        }

        if (used >= 3) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t i = 0; i < used; ++i) {
                sx += xs[i]; sy += ph[i];
                sxx += xs[i] * xs[i]; sxy += xs[i] * ph[i];
            }
            double nn = static_cast<double>(used);
            double den = nn * sxx - sx * sx;
            if (std::fabs(den) > 1e-12) {
                double slope     = (nn * sxy - sx * sy) / den;   // rad/symbol
                double intercept = (sy - slope * sx) / nn;       // rad at k=0
                best.freq_per_sym = static_cast<float>(slope);
                best.phase        = static_cast<float>(intercept);
            }
        }
    }

    best.ok = true;
    return best;
}

void DataAidedSync::derotateTracked(std::vector<std::complex<float>>& syms,
                                    const Result& r, float alpha) {
    if (!r.ok || syms.empty()) return;

    // Second-order loop: alpha corrects phase, beta corrects frequency. The
    // usual critically-damped relationship keeps it from ringing.
    const float beta = alpha * alpha * 0.25f;

    // Everything before sym_offset is pre-frame noise. Rotate it open-loop
    // and do NOT let it drive the loop: closing a decision-directed loop on
    // noise pulls it off-lock before the frame even starts, which showed up
    // as a correct frame header followed by all-ones garbage once the loop
    // had run away.
    {
        double p0 = static_cast<double>(r.phase)
                  - static_cast<double>(r.freq_per_sym) * r.sym_offset;
        size_t pre = std::min(syms.size(), static_cast<size_t>(std::max(r.sym_offset, 0)));
        for (size_t i = 0; i < pre; ++i) {
            double ang = -(p0 + static_cast<double>(r.freq_per_sym) * static_cast<double>(i));
            syms[i] *= std::complex<float>(static_cast<float>(std::cos(ang)),
                                           static_cast<float>(std::sin(ang)));
        }
    }

    double phase = static_cast<double>(r.phase);
    double freq  = static_cast<double>(r.freq_per_sym);

    for (size_t i = static_cast<size_t>(std::max(r.sym_offset, 0)); i < syms.size(); ++i) {
        auto& s = syms[i];
        std::complex<float> rot(static_cast<float>(std::cos(-phase)),
                                static_cast<float>(std::sin(-phase)));
        s *= rot;

        // BPSK decision: the ideal symbol is +/-1 on the real axis, so the
        // imaginary part of (symbol * decision) is the phase error. Using the
        // decision rather than a known reference is what lets this run
        // through the payload, where there is no reference.
        float d   = (s.real() >= 0.f) ? 1.f : -1.f;
        float err = d * s.imag();
        // Guard against a wild sample dragging the loop off-lock.
        if (err >  1.f) err =  1.f;
        if (err < -1.f) err = -1.f;

        freq  += static_cast<double>(beta)  * err;
        phase += static_cast<double>(alpha) * err + freq;
    }
}

void DataAidedSync::derotate(std::vector<std::complex<float>>& syms,
                             const Result& r) {
    if (!r.ok) return;
    for (size_t i = 0; i < syms.size(); ++i) {
        double k   = static_cast<double>(static_cast<long>(i) - r.sym_offset);
        double ang = -(static_cast<double>(r.phase) +
                       static_cast<double>(r.freq_per_sym) * k);
        syms[i] *= std::complex<float>(static_cast<float>(std::cos(ang)),
                                       static_cast<float>(std::sin(ang)));
    }
}

} // namespace sdr
