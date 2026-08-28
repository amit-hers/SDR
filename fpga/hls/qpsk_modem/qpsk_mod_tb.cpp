// C simulation for qpsk_mod_top.
//
// Feeds the payload bytes and compares the emitted IQ against the host's own
// RRCInterp output for the same bytes. The modulator's job is to reproduce the
// transmitted waveform, so that waveform is the reference.
//
// Exact equality is not the bar: the host works in float and the core in Q1.15
// through a 49-tap fixed-point FIR, so quantisation differences are expected.
// The bar is normalised correlation against the reference, which is what
// actually decides whether the peer can demodulate it.
//
// Three things are checked, and they fail differently on purpose:
//   sample count   -- rate control (16 samples per byte)
//   correlation    -- waveform construction (taps, phase order, constellation)
//   lag            -- group delay, i.e. drop-in equivalence with the host TX
#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The AXI4-Stream types must match qpsk_common.h exactly: ap_axiu, so the
// exported IP carries real TDATA/TKEEP/TSTRB/TLAST side channels instead of
// packing `last` into TDATA. I and Q share one 32-bit word.
typedef ap_axiu<32, 0, 0, 0> IQSample;   // data[15:0] = I, data[31:16] = Q
typedef ap_axiu<8, 0, 0, 0>  BitByte;    // data = one payload byte

void qpsk_mod_top(hls::stream<BitByte>&, hls::stream<IQSample>&,
                  volatile ap_uint<1>&, volatile ap_uint<2>&);

namespace {
std::vector<uint8_t> readFile(const std::string& p) {
    std::vector<uint8_t> v; FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    v.resize(n > 0 ? size_t(n) : 0);
    if (!v.empty() && std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    std::fclose(f); return v;
}
} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "../vectors";
    auto want_iq = readFile(dir + "/mod_ref.iq");
    auto bytes   = readFile(dir + "/mod_ref.bits");
    std::printf("qpsk_mod_top C simulation (reference = host RRCInterp output)\n");
    if (want_iq.empty() || bytes.empty()) {
        std::printf("  FAIL: vectors missing at %s (run gen_vectors)\n", dir.c_str());
        return 1;
    }

    hls::stream<BitByte>  in("in");
    hls::stream<IQSample> out("out");
    volatile ap_uint<1> en = 1;
    volatile ap_uint<2> bpsk = 0;

    std::vector<std::complex<double>> got;
    for (size_t n = 0; n < bytes.size(); ++n) {
        BitByte b; b.data = bytes[n];
        b.keep = -1; b.strb = -1;
        b.last = (n + 1 == bytes.size()) ? 1 : 0;
        in.write(b);
        qpsk_mod_top(in, out, en, bpsk);
        while (!out.empty()) {
            IQSample s = out.read();
            got.push_back({double((int16_t)(uint16_t)s.data.range(15,  0)),
                           double((int16_t)(uint16_t)s.data.range(31, 16))});
        }
    }
    const int16_t* w = reinterpret_cast<const int16_t*>(want_iq.data());
    const size_t nw = want_iq.size() / 4;
    std::printf("  emitted %zu samples, reference has %zu\n", got.size(), nw);

    // Rate: one byte is 4 QPSK symbols and each symbol is SPS=4 samples, so
    // the core owes exactly 16 samples per byte. Asserted, not just printed --
    // a rate fault and a waveform fault look nothing alike, and separating
    // them is what made the waveform bug findable.
    const size_t want_n = bytes.size() * 16;
    if (got.size() != want_n) {
        std::printf("  FAIL: expected %zu samples for %zu bytes\n", want_n, bytes.size());
        return 1;
    }

    // Amplitude. Correlation is scale-blind, so on its own it would pass a core
    // whose output was uniformly zero-scaled or hard against the rails. Both
    // have happened here: an earlier version multiplied by 32767 inside a
    // fixp_t that cannot hold it and emitted pure zeros, and the FIR still runs
    // through a saturating limiter. The measured peak is ~8352 LSB, a quarter
    // of full scale; the band below is wide enough not to care about tap
    // renormalisation and narrow enough to catch a dead or clipped output.
    double core_peak = 0;
    for (const auto& s : got)
        core_peak = std::max(core_peak, std::sqrt(s.real()*s.real() + s.imag()*s.imag()));
    std::printf("  peak |x| = %.0f LSB (%.1f%% of full scale)\n",
                core_peak, 100.0 * core_peak / 32767.0);
    if (core_peak < 2000.0 || core_peak > 31000.0) {
        std::printf("  FAIL: peak outside [2000, 31000] LSB -- output dead or clipping\n");
        return 1;
    }

    // Best normalised correlation over a lag search: the core's pulse-shaping
    // delay need not equal the host's.
    double best = 0.0; long bestlag = 0;
    const size_t n = std::min(got.size(), nw);
    for (long lag = -64; lag <= 64; ++lag) {
        double num = 0, ea = 0, eb = 0;
        for (size_t k = 64; k + 64 < n; ++k) {
            long j = long(k) + lag; if (j < 0 || size_t(j) >= nw) continue;
            double ai = got[k].real(), aq = got[k].imag();
            double bi = w[j * 2], bq = w[j * 2 + 1];
            num += ai * bi + aq * bq; ea += ai * ai + aq * aq; eb += bi * bi + bq * bq;
        }
        double c = (ea > 0 && eb > 0) ? num / std::sqrt(ea * eb) : 0.0;
        if (c > best) { best = c; bestlag = lag; }
    }
    std::printf("  best normalised correlation = %.6f at lag %ld\n", best, bestlag);

    // 0.999, not the 0.95 this started at. 0.95 was the bar for "close enough
    // to demodulate" while the core was being brought up; the measured figure
    // is 1.000000 to six places, because normalised correlation is second-order
    // insensitive to the Q1.15 quantisation that is the only difference left
    // between this and the host's float path. A bar three nines below a perfect
    // score leaves quantisation all the room it needs and still catches a
    // single wrong tap, a swapped I/Q, or an off-by-one in the delay line --
    // the last of which scored 0.0444 here.
    const double kMin = 0.999;
    if (best < kMin) {
        std::printf("  FAIL: correlation below %.3f\n", kMin);
        return 1;
    }
    // Lag 0 is structural, not luck: the fabric polyphase FIR and the host's
    // firinterp carry the same 6-symbol group delay because they run the same
    // 49 taps. Checking it keeps the core a drop-in for the host TX path. The
    // +-1 is slack for a pipeline change that shifts the output by a sample
    // without altering the waveform; anything larger means the delay line or
    // the phase order moved.
    if (bestlag < -1 || bestlag > 1) {
        std::printf("  FAIL: waveform matches but at lag %ld, expected 0\n", bestlag);
        return 1;
    }

    // Residual after removing the constant gain between the two paths (0.660:
    // RRC_H is the liquid prototype renormalised, and Q1.15 scales by 32768
    // against the host's 8192). This is the check correlation cannot do --
    // correlation is insensitive to a handful of bad samples in 4096, and a
    // per-sample bound is not. Measured: 5.62 LSB worst case on an 8355 LSB
    // peak, which is Q1.15 quantisation and nothing else.
    double num = 0, den = 0;
    for (size_t k = 0; k < n; ++k) {
        const double bi = w[k*2], bq = w[k*2+1];
        num += got[k].real()*bi + got[k].imag()*bq;
        den += bi*bi + bq*bq;
    }
    const double gain = (den > 0) ? num / den : 0.0;
    double maxerr = 0, refpeak = 0;
    for (size_t k = 0; k < n; ++k) {
        const double bi = w[k*2]*gain, bq = w[k*2+1]*gain;
        const double ei = got[k].real()-bi, eq = got[k].imag()-bq;
        maxerr  = std::max(maxerr,  std::sqrt(ei*ei + eq*eq));
        refpeak = std::max(refpeak, std::sqrt(bi*bi + bq*bq));
    }
    const double relerr = (refpeak > 0) ? maxerr / refpeak : 1.0;
    std::printf("  gain vs host = %.5f, worst per-sample error = %.2f LSB (%.3f%% of peak)\n",
                gain, maxerr, 100.0 * relerr);
    // 0.2%: three times the 0.067% the correct core measures, which is ample
    // headroom for Q1.15 rounding, and below the 0.425% that dropping a single
    // outermost tap (RRC_H[48], magnitude 0.001) produces. That mutant scores
    // 0.999996 on correlation -- it is exactly the class of defect a
    // scale-blind, energy-weighted metric cannot see and this one can.
    if (relerr > 0.002) {
        std::printf("  FAIL: per-sample error above 0.2%% of peak\n");
        return 1;
    }
    std::printf("  PASS\n"); return 0;
}
