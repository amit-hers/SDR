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
                   size_t max_search, Diag* diag) const {
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
    // Only retained when diagnostics are on -- the decode path allocates
    // nothing and behaves exactly as before.
    std::vector<float> coarse;
    if (diag) coarse.reserve(limit / COARSE_OFFSET_STEP + 1);
    for (size_t d = 0; d <= limit; d += COARSE_OFFSET_STEP) {
        float q = correlateAt(buf, d, ref_, ref_energy_, COARSE_REF_STRIDE);
        if (diag) coarse.push_back(q);
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

    if (diag) {
        diag->peak     = best.quality;
        diag->peak_off = best.offset;
        diag->searched = coarse.size();
        // Second peak, taken at least half a reference away from the winner.
        // Nearer than that is the same peak's own shoulder, not a rival: the
        // preamble is ~1120 samples long, so the correlation lobe is broad
        // and adjacent offsets are highly correlated by construction.
        const size_t sep = (L / 2) / COARSE_OFFSET_STEP + 1;
        const size_t win = static_cast<size_t>(coarse_best) / COARSE_OFFSET_STEP;
        diag->second     = 0.f;
        diag->second_off = -1;
        for (size_t i = 0; i < coarse.size(); ++i) {
            const size_t d = (i > win) ? (i - win) : (win - i);
            if (d < sep) continue;
            if (coarse[i] > diag->second) {
                diag->second     = coarse[i];
                diag->second_off = static_cast<long>(i * COARSE_OFFSET_STEP);
            }
        }
    }
    return best;
}

PreambleSync::Probe
PreambleSync::probe(const std::vector<std::complex<float>>& buf,
                    size_t max_search, double sample_rate,
                    double df_max_hz, int df_steps, size_t ref_stride) const {
    Probe p;
    const size_t L = ref_.size();
    if (buf.size() <= L || sample_rate <= 0.0 || df_steps < 1) return p;
    p.ran = true;

    size_t limit = buf.size() - L;
    if (max_search && max_search < limit) limit = max_search;

    // The stride MUST be coprime with the preamble's sample period, or the
    // probe silently lies.
    //
    // PREAMBLE_BYTE is 0xAA -- alternating bits, so the waveform repeats
    // every 2 symbols, i.e. every 2*sps samples. A stride that is a multiple
    // of that period samples the same phase of the repeat every time, the
    // strided reference collapses towards a constant, and it then correlates
    // strongly with anything carrying energy. Measured with stride 8 at
    // sps=4: 65 of 276 probes returned a "normalised" correlation above 1.0,
    // which is impossible, and the probe reported preambles that were not
    // there. The period is a power of two, so any ODD stride walks all
    // phases; round up to one.
    if (ref_stride < 1) ref_stride = 1;
    if ((ref_stride & 1u) == 0) ++ref_stride;

    // Coarser offset grid than find() uses: this is a "is anything here at
    // all" question, and the peak is broad. Exactness is find()'s job.
    // Rotating the REFERENCE by the trial offset is identical to derotating
    // the buffer -- the correlation is conj(ref)*buf -- but costs one pass
    // over 1120 samples per hypothesis instead of a cos/sin per multiply
    // inside the inner loop.
    const size_t step = 4;
    std::vector<std::complex<float>> ref_df(L);
    for (int i = 0; i < df_steps; ++i) {
        const double frac = (df_steps == 1) ? 0.0
                          : (2.0 * double(i) / double(df_steps - 1) - 1.0);
        const double df   = frac * df_max_hz;
        const double dphi = 2.0 * 3.14159265358979323846 * df / sample_rate;
        for (size_t k = 0; k < L; ++k) {
            const double a = dphi * static_cast<double>(k);
            const std::complex<double> rot(std::cos(a), std::sin(a));
            const std::complex<double> v =
                std::complex<double>(ref_[k].real(), ref_[k].imag()) * rot;
            ref_df[k] = std::complex<float>(static_cast<float>(v.real()),
                                            static_cast<float>(v.imag()));
        }
        float  bq = 0.f; long bo = 0;
        for (size_t d = 0; d <= limit; d += step) {
            float q = correlateAt(buf, d, ref_df, ref_energy_, ref_stride);
            if (q > bq) { bq = q; bo = static_cast<long>(d); }
        }
        if (bq > p.best_q) { p.best_q = bq; p.best_off = bo;
                             p.best_df = static_cast<float>(df); }
        if (df == 0.0)     { p.q_at_df0 = bq; p.off_at_df0 = bo; }
    }
    return p;
}

} // namespace sdr
