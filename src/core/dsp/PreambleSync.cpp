#include "sdr/dsp/PreambleSync.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/modem/Modem.hpp"
#include <cmath>
#include <algorithm>

namespace sdr {

PreambleSync::PreambleSync(int sps) {
    // Reference = preamble + sync word, exactly as the transmitter emits it.
    //
    // The preamble alone is useless for *timing*: PREAMBLE_BYTE (0xAA) is a
    // perfectly periodic bit pattern, so its correlation peaks identically
    // every 2 symbols. That detects a burst nicely but leaves the alignment
    // ambiguous modulo the period, and picking the wrong peak fails to
    // decode just as badly as not finding it. Appending the (non-periodic)
    // sync word breaks the ambiguity and makes the peak unique, and it also
    // means the returned offset is the true frame start.
    std::vector<uint8_t> ref_bytes(PREAMBLE_LEN, PREAMBLE_BYTE);
    ref_bytes.push_back(static_cast<uint8_t>((FRAME_SYNC >> 24) & 0xFF));
    ref_bytes.push_back(static_cast<uint8_t>((FRAME_SYNC >> 16) & 0xFF));
    ref_bytes.push_back(static_cast<uint8_t>((FRAME_SYNC >>  8) & 0xFF));
    ref_bytes.push_back(static_cast<uint8_t>( FRAME_SYNC        & 0xFF));

    Modem modem(ModScheme::BPSK);
    std::vector<std::complex<float>> syms;
    modem.modulate(ref_bytes.data(), static_cast<int>(ref_bytes.size()), syms);

    if (sps <= 1) {
        // Symbol-domain use: the caller is correlating against a stream that
        // is already one sample per symbol (post matched-filter/decimation),
        // so there is nothing to interpolate and no filter transient to drop.
        // liquid's firinterp rejects a factor of 1 outright, so this cannot
        // just fall through to RRCInterp.
        ref_ = syms;
    } else {
        RRCInterp interp(sps);
        interp.process(syms, ref_);

        // Drop the filter's start-up transient: those samples are shaped by
        // taps that were still filling and don't match what arrives
        // mid-burst.
        const size_t skip = static_cast<size_t>(RRC_TAPS);
        if (ref_.size() > skip * 2)
            ref_.erase(ref_.begin(), ref_.begin() + static_cast<long>(skip));
    }

    for (const auto& s : ref_)
        ref_energy_ += static_cast<double>(std::norm(s));
    ref_energy_ = std::sqrt(ref_energy_);
    if (ref_energy_ <= 0.0) ref_energy_ = 1.0;
}

namespace {

// One normalised correlation of ref against buf at offset d.
float correlateAt(const std::vector<std::complex<float>>& buf, size_t d,
                  const std::vector<std::complex<float>>& ref,
                  double ref_energy, size_t ref_stride) {
    const size_t L = ref.size();
    if (d + L > buf.size()) return 0.f;

    std::complex<double> acc(0.0, 0.0);
    double win_energy = 0.0;
    for (size_t k = 0; k < L; k += ref_stride) {
        const auto& r = ref[k];
        const auto& x = buf[d + k];
        // conj(ref) * buf, accumulated -- magnitude is phase-invariant, which
        // matters because BPSK carries an unknown carrier phase.
        acc += std::complex<double>(
            static_cast<double>(r.real()) * x.real() + static_cast<double>(r.imag()) * x.imag(),
            static_cast<double>(r.real()) * x.imag() - static_cast<double>(r.imag()) * x.real());
        win_energy += static_cast<double>(std::norm(x));
    }
    // ref_energy is over the full reference; scale it to the sampled subset.
    double ref_e = ref_energy / std::sqrt(static_cast<double>(ref_stride));
    double denom = std::sqrt(win_energy) * ref_e;
    return (denom > 0.0) ? static_cast<float>(std::abs(acc) / denom) : 0.f;
}

} // namespace

PreambleSync::Match
PreambleSync::find(const std::vector<std::complex<float>>& buf,
                   size_t max_search) const {
    Match best{0, 0.f, false};
    const size_t L = ref_.size();
    if (buf.size() <= L) return best;

    size_t limit = buf.size() - L;
    if (max_search && max_search < limit) limit = max_search;

    // Coarse pass: step across offsets and subsample the reference. The
    // correlation peak is broad enough (the preamble is ~1000 samples) to
    // survive both, and this cuts the work by COARSE_OFFSET_STEP *
    // COARSE_REF_STRIDE versus correlating every offset at full resolution.
    // Subsampling the *reference* shifts the correlation peak enough to cost
    // real decodes (symbol timing needs a sample-exact offset), so only the
    // offset grid is coarsened; the reference stays full-resolution.
    constexpr size_t COARSE_OFFSET_STEP = 2;
    constexpr size_t COARSE_REF_STRIDE  = 1;

    size_t coarse_best = 0;
    float  coarse_q    = 0.f;
    for (size_t d = 0; d <= limit; d += COARSE_OFFSET_STEP) {
        float q = correlateAt(buf, d, ref_, ref_energy_, COARSE_REF_STRIDE);
        if (q > coarse_q) { coarse_q = q; coarse_best = d; }
    }

    // Fine pass: full-resolution correlation in a small neighbourhood of the
    // coarse peak. Symbol timing is sensitive at sample granularity, so the
    // returned offset has to be exact even though finding the region wasn't.
    size_t lo = (coarse_best > COARSE_OFFSET_STEP * 2)
              ? coarse_best - COARSE_OFFSET_STEP * 2 : 0;
    size_t hi = std::min(limit, coarse_best + COARSE_OFFSET_STEP * 2);
    for (size_t d = lo; d <= hi; ++d) {
        float q = correlateAt(buf, d, ref_, ref_energy_, 1);
        if (q > best.quality) { best.quality = q; best.offset = static_cast<long>(d); }
    }

    best.found = best.quality >= MIN_QUALITY;
    return best;
}

} // namespace sdr
