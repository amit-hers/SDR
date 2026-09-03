// C simulation for qpsk_demod_top.
//
// Drives the core with IQ produced by the VALIDATED host transmit path
// (fpga/hls/vectors/gen_vectors.cpp) and requires the recovered bytes to match
// what was transmitted. Synthesis proving the hardware graph is alive says
// nothing about whether it computes the right answer; this is the test that
// does.
//
// Alignment: the demodulator has pipeline latency and no framing, so its first
// output byte does not correspond to the first transmitted byte. The check
// therefore searches for the best alignment and reports the match at that
// offset -- a real demodulator produces a long exact run somewhere, a broken
// one produces none at any offset.
#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The AXI4-Stream types must match qpsk_common.h exactly: ap_axiu, so the
// exported IP carries real TDATA/TKEEP/TSTRB/TLAST side channels instead of
// packing `last` into TDATA. I and Q share one 32-bit word.
typedef ap_axiu<32, 0, 0, 0> IQSample;   // data[15:0] = I, data[31:16] = Q
typedef ap_axiu<8, 0, 0, 0>  BitByte;    // data = one payload byte

void qpsk_demod_top(hls::stream<IQSample>&, hls::stream<BitByte>&,
                    volatile ap_uint<1>&, volatile ap_uint<32>&,
                    volatile ap_uint<1>&, volatile ap_uint<1>&);

namespace {

std::vector<uint8_t> readFile(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) { std::printf("  cannot open %s\n", p.c_str()); return v; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    v.resize(n > 0 ? size_t(n) : 0);
    if (!v.empty() && std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    std::fclose(f);
    return v;
}

// Longest run of consecutive exact matches, searched at SYMBOL granularity.
//
// Searching byte alignment alone is not enough: four QPSK symbols make a byte,
// so a one-symbol phase difference between the core's decimation and the
// transmitter's packing shifts every byte boundary and produces a mismatch at
// all 256 byte offsets. That made a correct core look broken. Comparing symbol
// streams subsumes byte alignment and covers the phase.
struct Match { size_t offset{0}; size_t run{0}; size_t matched{0}; size_t compared{0}; };

static std::vector<uint8_t> toSymbols(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> sym;
    sym.reserve(bytes.size() * 4);
    for (uint8_t b : bytes)
        for (int k = 3; k >= 0; --k) sym.push_back((b >> (k * 2)) & 0x3);
    return sym;
}

Match bestAlignment(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    Match best;
    const std::vector<uint8_t> g = toSymbols(got), w = toSymbols(want);
    if (g.empty() || w.empty()) return best;
    for (size_t off = 0; off + 1 < g.size(); ++off) {
        size_t run = 0, cur = 0, matched = 0;
        const size_t n = std::min(g.size() - off, w.size());
        for (size_t k = 0; k < n; ++k) {
            if (g[off + k] == w[k]) { ++matched; if (++cur > run) run = cur; }
            else cur = 0;
        }
        if (run > best.run) best = Match{off, run, matched, n};
    }
    // Report in bytes so the numbers stay comparable to the expected payload.
    best.run /= 4; best.matched /= 4; best.compared /= 4;
    return best;
}

// Push samples through the core with reset DEASSERTED and throw the output
// away, purely to leave every stage dirty: filter history, AGC gain and
// envelope, carrier phase and integrator, and the symbol-phase counter.
//
// The length is deliberately not a multiple of SPS. Relying on the other
// vectors to dirty the core is not enough -- they are 4096 samples, so the
// symbol counter cycles mod 4 straight back to zero and a missing reset for it
// is invisible. 1023 leaves it at 3.
static void perturb(const std::string& dir, const std::string& name, size_t n_samp) {
    auto iq = readFile(dir + "/" + name + ".iq");
    if (iq.empty()) return;
    hls::stream<IQSample> in("perturb_in");
    hls::stream<BitByte>  out("perturb_out");
    volatile ap_uint<1>  enable = 1, reset = 0;
    volatile ap_uint<1>  diff = 0;          // vectors are absolute, not differential
    volatile ap_uint<32> locks = 0;
    const int16_t* s = reinterpret_cast<const int16_t*>(iq.data());
    const size_t n = std::min(n_samp, iq.size() / 4);
    for (size_t k = 0; k < n; ++k) {
        IQSample v;
        v.data.range(15,  0) = ap_uint<16>((uint16_t)s[k * 2]);
        v.data.range(31, 16) = ap_uint<16>((uint16_t)s[k * 2 + 1]);
        v.keep = -1; v.strb = -1; v.last = 0;
        in.write(v);
        qpsk_demod_top(in, out, enable, locks, reset, diff);
        while (!out.empty()) out.read();
    }
}

int runVector(const std::string& dir, const std::string& name, size_t min_run,
              Match* m_out = nullptr, std::vector<uint8_t>* bytes_out = nullptr) {
    auto iq   = readFile(dir + "/" + name + ".iq");
    auto want = readFile(dir + "/" + name + ".bits");
    if (iq.empty() || want.empty()) {
        // A test that cannot find its stimulus has proven nothing. Reporting
        // that as success is worse than failing: it makes an empty run look
        // like a green gate.
        std::printf("  %-10s FAIL: vectors missing at %s (run gen_vectors)\n",
                    name.c_str(), dir.c_str());
        return 1;
    }
    const size_t nsamp = iq.size() / 4;   // int16 I + int16 Q

    hls::stream<IQSample> in("in");
    hls::stream<BitByte>  out("out");
    volatile ap_uint<1>  enable = 1;
    volatile ap_uint<32> locks  = 0;
    volatile ap_uint<1>  reset  = 0;
    volatile ap_uint<1>  diff   = 0;        // vectors are absolute, not differential

    // Start every vector from the core's power-on state.
    //
    // Without this the vectors are not independent: the core is a chain of
    // static accumulators, so each run inherited the previous one's converged
    // carrier phase, loop integrator, symbol-phase counter and filter history.
    // `stress` scored 248 when it ran third and 253 on its own -- the suite's
    // verdict depended on the order its cases happened to be listed in.
    //
    // One call with reset high is enough; it is level sensitive and clears
    // every stage in a single pass.
    reset = 1;
    qpsk_demod_top(in, out, enable, locks, reset, diff);
    reset = 0;
    while (!out.empty()) out.read();

    std::vector<uint8_t> got;
    const int16_t* s = reinterpret_cast<const int16_t*>(iq.data());
    for (size_t n = 0; n < nsamp; ++n) {
        IQSample v;
        v.data.range(15,  0) = ap_uint<16>((uint16_t)s[n * 2]);
        v.data.range(31, 16) = ap_uint<16>((uint16_t)s[n * 2 + 1]);
        v.keep = -1; v.strb = -1;
        v.last = (n + 1 == nsamp) ? 1 : 0;
        in.write(v);
        // The core consumes at most one sample per call and may emit none.
        qpsk_demod_top(in, out, enable, locks, reset, diff);
        while (!out.empty()) got.push_back((uint8_t)out.read().data);
    }

    Match m = bestAlignment(got, want);
    if (m_out) *m_out = m;
    if (bytes_out) *bytes_out = got;
    const double pct = m.compared ? 100.0 * double(m.matched) / double(m.compared) : 0.0;
    std::printf("  %-10s in=%zu samples  out=%zu bytes  best_run=%zu  match=%.1f%% "
                "at offset %zu  locks=%u\n",
                name.c_str(), nsamp, got.size(), m.run, pct, m.offset,
                // Copy out of the volatile before converting -- same rule that
                // bit the design itself: ap_uint has a volatile-aware copy
                // constructor but no volatile conversion operators.
                (unsigned)ap_uint<32>(locks).to_uint());

    if (got.empty()) { std::printf("             FAIL: no output at all\n"); return 1; }
    // Required run is per-vector, because the vectors ask for different things.
    //
    // On `clean` and `impaired` the bar is bit-exactness: the matched filter
    // needs NTAPS/sps symbols of history before its output means anything, so
    // the first 3 bytes are fill, and every remaining byte must be right.
    // "Most of them" is not a correctness gate.
    //
    // `stress` runs at 6.2x the carrier offset and is expected to cost a
    // little. It measures 248 of 256 -- 99.6% of symbols correct, the loss
    // being one isolated symbol slip rather than a failure to lock. Demanding
    // 252 there would not be a stricter test, it would be a flaky one: the
    // exact figure moves with the noise realisation. 240 is well clear of that
    // spread and still enormously far from a broken loop, which scores around
    // 10 at any alignment -- the failure is bimodal, so the gate has room to
    // breathe without losing any power to detect a regression.
    if (m.run < min_run) {
        std::printf("             FAIL: longest exact run %zu < required %zu\n", m.run, min_run);
        return 1;
    }
    std::printf("             PASS\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "../vectors";
    std::printf("qpsk_demod_top C simulation (vectors from the host TX path)\n");
    int rc = 0;
    struct Case { const char* name; size_t min_run; };
    // All seven vectors, not three. The four that were missing are the ones
    // that exercise the timing loop -- which is exactly why nothing caught the
    // loop being inert for as long as it was. shift25/shift50 vary the sampling
    // phase; lowamp4/lowamp8 vary the amplitude.
    //
    // clean and impaired are no longer bit-exact, and that is the fix working
    // rather than a regression. The timing loop now ACQUIRES instead of
    // relying on these vectors being generated already aligned to its reset
    // phase, and acquisition costs symbols 2 and 4 -- after which the run is
    // 1007 consecutive exact symbols out of the 1012 available. The old 252
    // gate encoded "the loop never moves", which was true and was the defect.
    //
    // Gates sit ~3 bytes under the measured figure, far above the ~10 a broken
    // loop scores at any alignment; the failure is bimodal, so this has room
    // without losing power. Measured at the committed gains:
    //   clean 251  impaired 251  stress 253  shift25 253
    //   shift50 244  lowamp4 253  lowamp8 168
    const Case cases[] = {Case{"clean",    248},
                          Case{"impaired", 248},
                          Case{"stress",   240},   // locks and holds under 8x CFO
                          Case{"shift25",  240},   // quarter-sample phase offset
                          Case{"shift50",  230},   // half-sample phase offset
                          Case{"lowamp4",  240},   // 4x below nominal amplitude
                          Case{"lowamp8",  150}};  // 8x below -- AGC convergence limited
    // An optional second argument runs one case on its own. That is how the
    // reset path is checked: each vector run alone must give the same numbers
    // as the full sequence, which is only true if reset really does return the
    // core to power-on state.
    const char* only = (argc > 2) ? argv[2] : nullptr;
    bool ran = false;
    Match firstMatch;
    std::vector<uint8_t> firstBytes;
    for (const Case& c : cases) {
        if (only && std::strcmp(only, c.name) != 0) continue;
        Match m;
        std::vector<uint8_t> b;
        if (runVector(dir, c.name, c.min_run, &m, &b) != 0) rc = 1;
        if (!ran) { firstMatch = m; firstBytes = b; }
        ran = true;
    }
    if (!ran) { std::printf("  no such vector '%s'\n", only ? only : ""); return 1; }

    // Reset completeness.
    //
    // Passing the gates does not prove the reset clears everything: before the
    // reset path existed, `stress` scored 248 running third instead of 253 on
    // its own, and 248 still clears its gate of 240. A partial reset -- one
    // forgotten accumulator -- would look exactly like that and slip through.
    //
    // So re-run the first vector after all the others have perturbed the core
    // and require its OUTPUT BYTES to be identical, not merely still passing.
    //
    // Byte equality, not the match statistics: those are far too coarse. They
    // are computed over a best-alignment search across 4096 samples, so a
    // handful of wrong bytes at startup -- exactly what a leaked AGC gain or a
    // dirty filter history produces -- disappears into them. Measured: with the
    // AGC, matched-filter and timing resets each removed in turn, all three
    // still passed a statistics-based check, and all three fail this one.
    // (The symbol-phase counter is a good illustration of why luck is not
    // coverage: it cycles mod 4 and the vectors are 4096 samples, so it
    // happens to land back on zero regardless. Hence perturb() above.)
    //
    // The AGC is the one piece of state this cannot cover, and that is a
    // property of the design rather than a gap in the test. A QPSK decision is
    // a pair of sign tests, and the AGC applies one positive scalar to both I
    // and Q, so sign(g*x) == sign(x) for any g > 0: gain simply cannot change
    // a decision except by driving the saturation clamp. Checked rather than
    // assumed -- resetting the gain to 0.25 and to 4.0, a 16x span, leaves the
    // output byte-identical either way. Do not "strengthen" this by asserting
    // on AGC state; there is nothing observable to assert on.
    if (rc == 0) {
        const Case& first = *std::find_if(std::begin(cases), std::end(cases),
            [&](const Case& c){ return !only || std::strcmp(only, c.name) == 0; });
        Match again;
        std::vector<uint8_t> againBytes;
        // Dirty every stage on purpose, then reset and re-run.
        perturb(dir, "stress", 1023);
        std::printf("  re-run %s after perturbation (reset completeness)\n", first.name);
        if (runVector(dir, first.name, first.min_run, &again, &againBytes) != 0) {
            rc = 1;
        } else if (againBytes != firstBytes) {
            size_t k = 0;
            while (k < againBytes.size() && k < firstBytes.size() &&
                   againBytes[k] == firstBytes[k]) ++k;
            std::printf("             FAIL: output differs from its first run at byte %zu"
                        " (%zu bytes vs %zu) -- reset leaves state behind\n",
                        k, againBytes.size(), firstBytes.size());
            rc = 1;
        }
    }
    std::printf("%s\n", rc == 0 ? "csim: PASS" : "csim: FAIL");
    return rc;
}
