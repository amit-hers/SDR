#include "sdr/dsp/FixedTimingSync.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include <algorithm>
#include <cmath>

namespace sdr {

namespace {

constexpr int Q = 15;                    // Q1.15
constexpr int32_t ONE_Q15 = 1 << Q;

inline int16_t satQ15(int64_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

// Root-raised-cosine prototype, same rolloff/span the transmitter shapes with.
// Generated here rather than borrowed from liquid so the coefficient set that
// goes into the HDL ROM is exactly what this model used.
std::vector<double> rrcTaps(int sps, int span, double beta) {
    const int n = sps * span + 1;
    std::vector<double> h(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double t = (i - (n - 1) / 2.0) / sps;
        double v;
        if (std::fabs(t) < 1e-9) {
            v = 1.0 - beta + 4.0 * beta / M_PI;
        } else if (std::fabs(std::fabs(4.0 * beta * t) - 1.0) < 1e-9) {
            v = beta / std::sqrt(2.0) *
                ((1.0 + 2.0 / M_PI) * std::sin(M_PI / (4.0 * beta)) +
                 (1.0 - 2.0 / M_PI) * std::cos(M_PI / (4.0 * beta)));
        } else {
            double num = std::sin(M_PI * t * (1.0 - beta)) +
                         4.0 * beta * t * std::cos(M_PI * t * (1.0 + beta));
            double den = M_PI * t * (1.0 - 16.0 * beta * beta * t * t);
            v = num / den;
        }
        h[static_cast<size_t>(i)] = v;
    }
    double e = 0.0;
    for (double v : h) e += v * v;
    e = std::sqrt(e);
    for (double& v : h) v /= e;          // unit energy
    return h;
}

} // namespace

FixedTimingSync::FixedTimingSync(const Config& cfg) : cfg_(cfg) {
    auto h = rrcTaps(cfg_.sps, RRC_TAPS / cfg_.sps, RRC_ROLLOFF);
    taps_q15_.resize(h.size());
    for (size_t i = 0; i < h.size(); ++i)
        taps_q15_[i] = satQ15(std::llround(h[i] * ONE_Q15));

    if (cfg_.interp == Config::Interp::POLYPHASE) {
        // Design the RRC at sps*P samples per symbol, then deal the taps out
        // round-robin into P sub-filters. Sub-filter p is the matched filter
        // delayed by p/P of a sample, so selecting a phase performs matched
        // filtering and fractional interpolation in one pass -- there is no
        // separate FIR stage in this mode.
        const int P    = std::max(2, cfg_.n_phases);
        const int span = RRC_TAPS / cfg_.sps;
        auto proto = rrcTaps(cfg_.sps * P, span, RRC_ROLLOFF);
        taps_per_phase_ = RRC_TAPS;
        bank_q15_.assign(static_cast<size_t>(P) * taps_per_phase_, 0);
        for (int p = 0; p < P; ++p) {
            // Normalise each phase to unit energy so switching phase does not
            // step the output level (which would disturb the AGC downstream).
            std::vector<double> ph(static_cast<size_t>(taps_per_phase_), 0.0);
            double e = 0.0;
            for (int k = 0; k < taps_per_phase_; ++k) {
                size_t idx = static_cast<size_t>(k) * P + static_cast<size_t>(p);
                double v = (idx < proto.size()) ? proto[idx] : 0.0;
                ph[static_cast<size_t>(k)] = v;
                e += v * v;
            }
            e = (e > 0) ? std::sqrt(e) : 1.0;
            for (int k = 0; k < taps_per_phase_; ++k)
                bank_q15_[static_cast<size_t>(p) * taps_per_phase_ + k] =
                    satQ15(std::llround(ph[static_cast<size_t>(k)] / e * ONE_Q15));
        }
    }
}

FixedTimingSync::Result
FixedTimingSync::process(const std::vector<std::complex<float>>& in) {
    Result r;
    const size_t N = in.size();
    const int    L = static_cast<int>(taps_q15_.size());
    if (N < static_cast<size_t>(L + 8)) return r;

    // ── Quantise input to Q1.15 ───────────────────────────────────────────
    // The ADC delivers 12 bits; the daemon scales to +/-1.0. Anything beyond
    // full scale clips, exactly as it would in fabric.
    std::vector<int16_t> xi(N), xq(N);
    for (size_t i = 0; i < N; ++i) {
        xi[i] = satQ15(std::llround(in[i].real() * ONE_Q15));
        xq[i] = satQ15(std::llround(in[i].imag() * ONE_Q15));
    }

    // ── Matched filter: 32-tap FIR, Q1.15 x Q1.15 -> Q2.30, 40-bit accum ──
    // In fabric this is a transposed-form systolic chain; here the loop is
    // written straight because only the arithmetic has to match.
    const bool poly = (cfg_.interp == Config::Interp::POLYPHASE);
    std::vector<int16_t> yi(N), yq(N);
    const int64_t acc_max =  (int64_t(1) << (cfg_.acc_bits - 1)) - 1;
    const int64_t acc_min = -(int64_t(1) << (cfg_.acc_bits - 1));
    for (size_t n = 0; poly ? false : (n < N); ++n) {
        int64_t ai = 0, aq = 0;
        int kmax = std::min<int>(L, static_cast<int>(n) + 1);
        for (int k = 0; k < kmax; ++k) {
            int64_t c = taps_q15_[static_cast<size_t>(k)];
            ai += c * xi[n - static_cast<size_t>(k)];
            aq += c * xq[n - static_cast<size_t>(k)];
        }
        if (ai > acc_max || ai < acc_min || aq > acc_max || aq < acc_min) ++r.sat_count;
        ai = std::clamp(ai, acc_min, acc_max);
        aq = std::clamp(aq, acc_min, acc_max);
        yi[n] = satQ15(ai >> Q);          // back to Q1.15
        yq[n] = satQ15(aq >> Q);
    }

    // ── Timing recovery: cubic Farrow interpolator + Gardner TED + PI NCO ─
    // The NCO accumulator holds the fractional input index of the next
    // symbol. Advancing it by sps plus the loop correction is the same
    // operation a hardware phase accumulator performs.
    const double sps   = cfg_.sps;
    const double bw    = cfg_.loop_bw;
    const double alpha = bw;                // proportional
    const double beta  = bw * bw / 4.0;     // integral, critically damped

    // Cubic Lagrange through x[-1..2], evaluated at mu in [0,1). Written in
    // Farrow form: four fixed sub-filter outputs combined by a Horner chain
    // in mu, which is 3 multiplies and no coefficient memory.
    auto farrow = [](const std::vector<int16_t>& s, int base, double mu) -> double {
        double xm1 = s[static_cast<size_t>(base - 1)];
        double x0  = s[static_cast<size_t>(base)];
        double x1  = s[static_cast<size_t>(base + 1)];
        double x2  = s[static_cast<size_t>(base + 2)];
        double c3 = (-xm1 + 3.0 * x0 - 3.0 * x1 + x2) / 6.0;
        double c2 = ( xm1 - 2.0 * x0 + x1) / 2.0;
        double c1 = (-2.0 * xm1 - 3.0 * x0 + 6.0 * x1 - x2) / 6.0;
        double c0 = x0;
        return ((c3 * mu + c2) * mu + c1) * mu + c0;
    };

    // In polyphase mode the bank consumes the raw input, so the working
    // arrays are the unfiltered samples.
    const std::vector<int16_t>& wi = poly ? xi : yi;
    const std::vector<int16_t>& wq = poly ? xq : yq;
    const int P = std::max(2, cfg_.n_phases);

    // Polyphase sample: pick the phase nearest mu, then run that sub-filter.
    int last_phase = 0;
    auto polySample = [&](int base, double mu) -> std::complex<double> {
        int p = static_cast<int>(std::llround(mu * P)) % P;
        if (p < 0) p += P;
        last_phase = p;
        const int16_t* c = &bank_q15_[static_cast<size_t>(p) * taps_per_phase_];
        int64_t ai = 0, aq = 0;
        for (int k = 0; k < taps_per_phase_; ++k) {
            int idx = base - k;
            if (idx < 0) break;
            ai += static_cast<int64_t>(c[k]) * wi[static_cast<size_t>(idx)];
            aq += static_cast<int64_t>(c[k]) * wq[static_cast<size_t>(idx)];
        }
        return { double(ai >> Q), double(aq >> Q) };
    };

    // ── Fixed-point loop state (Q16.16), used when cfg_.fixed_loop ────────
    // pos_q and freq_q are what an HDL phase accumulator holds. alpha/beta
    // become right-shifts, and the power normalisation becomes a barrel
    // shift driven by the position of the leading one -- all synthesisable.
    constexpr int LQ  = 16;   // NCO fraction
    constexpr int EQ  = 24;   // error fraction -- must exceed LQ, see below
    constexpr int FQ  = 32;   // integrator fraction
    const int64_t sps_q = static_cast<int64_t>(sps) << LQ;
    int64_t pos_q  = (static_cast<int64_t>(L) << LQ) + sps_q;
    int64_t freq_q = 0;       // Q16.32
    // Loop gains as shifts. The integrator is carried at Q16.32, not Q16.16:
    // beta*err for a typical err of 1e-3 is ~1e-8 of a sample, which is three
    // orders of magnitude below a Q16.16 LSB. Accumulating it at the NCO's
    // own precision truncates every update to zero and silently reduces the
    // loop to proportional-only -- measured as 0% CRC with the symbol count
    // drifting 75 symbols per burst.
    const int alpha_sh = cfg_.alpha_sh;
    const int beta_sh  = cfg_.beta_sh;

    r.syms.reserve(N / static_cast<size_t>(sps) + 8);
    r.idx .reserve(N / static_cast<size_t>(sps) + 8);

    double pos  = static_cast<double>(L) + sps;   // past the filter transient
    double freq = 0.0;
    std::complex<double> prev{0.0, 0.0};
    bool have_prev = false;

    while ((cfg_.fixed_loop ? (double(pos_q) / (1 << LQ)) : pos) + sps + 2.0
           < static_cast<double>(N)) {
        int    base;
        double mu;
        if (cfg_.fixed_loop) {
            base = static_cast<int>(pos_q >> LQ);
            mu   = double(pos_q & ((int64_t(1) << LQ) - 1)) / (1 << LQ);
        } else {
            base = static_cast<int>(std::floor(pos));
            mu   = pos - base;
        }
        if (base < 1 || base + 2 >= static_cast<int>(N)) break;

        int last_hb = -1, last_hphase = -1;
        std::complex<double> cur = poly ? polySample(base, mu)
                                        : std::complex<double>{ farrow(wi, base, mu),
                                                                farrow(wq, base, mu) };
        const int cur_phase = last_phase;

        // Output sample, taken at a fixed offset from the loop's lock point.
        // With phase_offset = 0 this is the same instant the TED uses.
        std::complex<double> out = cur;
        if (cfg_.phase_offset != 0.0) {
            double op = pos + cfg_.phase_offset * sps;
            int    ob = static_cast<int>(std::floor(op));
            double omu = op - ob;
            if (ob >= 1 && ob + 2 < static_cast<int>(N))
                out = poly ? polySample(ob, omu)
                           : std::complex<double>{ farrow(wi, ob, omu), farrow(wq, ob, omu) };
        }

        // Gardner: the error uses the sample halfway between this symbol and
        // the previous one, and needs no symbol decisions -- which is why it
        // suits a block that runs before carrier recovery.
        double last_raw_e = 0.0, last_pwr = 0.0;
        double last_mid_i = 0.0, last_mid_q = 0.0;
        int64_t t_pos_before=pos_q, t_pos_after=pos_q, t_freq_before=freq_q,
                t_freq_after=freq_q, t_freq_shift=0, t_e_alpha=0, t_e_beta=0;
        long long last_e_q = 0;
        double err = 0.0;
        if (have_prev) {
            double hp   = (cfg_.fixed_loop ? double(pos_q) / (1 << LQ) : pos) - sps / 2.0;
            int    hb   = static_cast<int>(std::floor(hp));
            double hmu  = hp - hb;
            if (hb >= 1 && hb + 2 < static_cast<int>(N)) {
                last_hb = hb;
                std::complex<double> mid = poly ? polySample(hb, hmu)
                                                : std::complex<double>{ farrow(wi, hb, hmu),
                                                                        farrow(wq, hb, hmu) };
                last_hphase = last_phase;
                last_mid_i = mid.real(); last_mid_q = mid.imag();
                err = mid.real() * (cur.real() - prev.real()) +
                      mid.imag() * (cur.imag() - prev.imag());
                // Normalise by signal power, not by a fixed constant. The raw
                // product scales with amplitude squared, so a fixed divisor
                // makes the loop gain depend on how hard the AGC happens to be
                // driving -- at burst start the symbols sit near 0.01 of full
                // scale and the correction collapses to ~1e-5, leaving the NCO
                // free-running. In fabric this is the usual reciprocal-of-power
                // scaling, or a shift derived from the AGC gain word.
                double p = 0.5 * (std::norm(cur) + std::norm(prev));
                last_raw_e = err;
                last_pwr   = p;
                err = (p > 1.0) ? (err / p) : 0.0;
            }
        }

        // Negative feedback: a positive Gardner error means we sampled late,
        // so the next symbol instant must move earlier. Feeding it back with
        // the wrong sign still holds the correct symbol *rate* -- the loop
        // locks -- but it locks onto the eye crossing instead of the eye
        // centre, so symbol count and spacing look perfect while every
        // decision is taken at the point of maximum ISI.
        if (cfg_.fixed_loop) {
            // err is dimensionless after power normalisation; carry it as
            // Q16.16 and apply the loop gains as shifts.
            int64_t e_q = static_cast<int64_t>(err * (int64_t(1) << EQ));
            const int64_t lim = int64_t(1) << EQ;                 // +-1.0
            if (e_q >  lim) e_q =  lim;
            if (e_q < -lim) e_q = -lim;
            last_e_q = e_q;
            t_pos_before  = pos_q;
            t_freq_before = freq_q;
            t_e_beta      = (beta_sh >= 0) ? (e_q >> beta_sh) : (e_q << -beta_sh);
            freq_q       -= t_e_beta;
            t_freq_after  = freq_q;
            t_freq_shift  = freq_q >> (FQ - LQ);
            t_e_alpha     = e_q >> alpha_sh;
            pos_q        += sps_q + t_freq_shift - t_e_alpha;

            // Bound the per-symbol advance to [sps-1, sps+1] samples.
            //
            // Defensive only: this was added chasing a "zero advance
            // duplicates a symbol" theory that turned out to be an artifact
            // of the diagnostic tool, not a real event. Traces show the loop
            // only ever steps 3, 4 or 5 samples, and the 3s and 5s are
            // balanced (285 vs 280 at 2 MHz), i.e. mu dithering across the
            // wrap boundary with no net drift. This clamp therefore fixes no
            // measured defect and changed no measured result.
            //
            // It is kept because +-1 sample per symbol is already far more
            // correction authority than any real clock offset needs, so
            // bounding it costs nothing and makes a pathological jump
            // unrepresentable. Do not read it as a fix for anything observed.
            //
            // Worth noting separately: the loop locks with mu pinned at
            // 0.94-0.99, hard against the wrap boundary. That is a genuine
            // fragility and is not addressed here.
            {
                const int64_t adv     = pos_q - t_pos_before;
                const int64_t min_adv = (static_cast<int64_t>(sps) - 1) << LQ;
                const int64_t max_adv = (static_cast<int64_t>(sps) + 1) << LQ;
                if      (adv < min_adv) pos_q = t_pos_before + min_adv;
                else if (adv > max_adv) pos_q = t_pos_before + max_adv;
            }
            t_pos_after   = pos_q;
        } else {
            freq -= beta * err;
            pos  += sps + freq - alpha * err;
        }

        r.syms.push_back({ static_cast<float>(out.real() / ONE_Q15),
                           static_cast<float>(out.imag() / ONE_Q15) });
        r.idx.push_back(base);            // functional: used to map windows
        if (cfg_.trace) {
            r.mu.push_back(mu);
            r.phase.push_back(cur_phase);
            r.hidx.push_back(last_hb);
            r.hphase.push_back(last_hphase);
            r.raw_e.push_back(last_raw_e);
            r.pwr.push_back(last_pwr);
            r.e_q.push_back(last_e_q);
            r.cur_i.push_back(cur.real());   r.cur_q.push_back(cur.imag());
            r.prev_i.push_back(prev.real()); r.prev_q.push_back(prev.imag());
            r.mid_i.push_back(last_mid_i);   r.mid_q.push_back(last_mid_q);
            r.pos_before.push_back(t_pos_before); r.pos_after.push_back(t_pos_after);
            r.sps_term.push_back(sps_q);
            r.freq_before.push_back(t_freq_before); r.freq_after.push_back(t_freq_after);
            r.freq_shifted.push_back(t_freq_shift);
            r.e_alpha.push_back(t_e_alpha); r.e_beta.push_back(t_e_beta);
        }
        prev = cur;
        have_prev = true;
    }
    return r;
}

} // namespace sdr
