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

    // Residual frequency shows up as a phase difference between the first and
    // second halves of the reference. A single static phase estimate is not
    // enough: two free-running 0.5ppm oscillators at 434 MHz can differ by a
    // few hundred Hz, which rotates the constellation appreciably across even
    // one short frame.
    const size_t half = L / 2;
    std::complex<double> a1(0.0, 0.0), a2(0.0, 0.0);
    for (size_t k = 0; k < half; ++k) {
        const auto& x = syms[static_cast<size_t>(best.sym_offset) + k];
        const auto& r = ref_[k];
        a1 += std::complex<double>(x.real(), x.imag()) *
              std::conj(std::complex<double>(r.real(), r.imag()));
    }
    for (size_t k = half; k < L; ++k) {
        const auto& x = syms[static_cast<size_t>(best.sym_offset) + k];
        const auto& r = ref_[k];
        a2 += std::complex<double>(x.real(), x.imag()) *
              std::conj(std::complex<double>(r.real(), r.imag()));
    }
    if (std::abs(a1) > 0.0 && std::abs(a2) > 0.0) {
        double dphi = std::arg(a2 * std::conj(a1));
        best.freq_per_sym = static_cast<float>(dphi / static_cast<double>(half));
        // Re-centre phase on the start of the reference.
        best.phase -= best.freq_per_sym * static_cast<float>(half) * 0.5f;
    }

    best.ok = true;
    return best;
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
