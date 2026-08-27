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
#include "ap_int.h"
#include "hls_stream.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct IQSample { ap_int<16> i, q; ap_uint<1> last; };
struct BitByte  { ap_uint<8> data; ap_uint<1> valid, last; };

void qpsk_demod_top(hls::stream<IQSample>&, hls::stream<BitByte>&,
                    volatile ap_uint<1>&, volatile ap_uint<32>&);

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

// Longest run of consecutive exact byte matches over every alignment.
struct Match { size_t offset{0}; size_t run{0}; size_t matched{0}; size_t compared{0}; };

Match bestAlignment(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    Match best;
    if (got.empty() || want.empty()) return best;
    for (size_t off = 0; off + 1 < got.size(); ++off) {
        size_t run = 0, matched = 0, n = std::min(got.size() - off, want.size());
        size_t cur = 0;
        for (size_t k = 0; k < n; ++k) {
            if (got[off + k] == want[k]) { ++matched; ++cur; if (cur > run) run = cur; }
            else cur = 0;
        }
        if (run > best.run) best = Match{off, run, matched, n};
    }
    return best;
}

int runVector(const std::string& dir, const std::string& name) {
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

    std::vector<uint8_t> got;
    const int16_t* s = reinterpret_cast<const int16_t*>(iq.data());
    for (size_t n = 0; n < nsamp; ++n) {
        IQSample v; v.i = s[n * 2]; v.q = s[n * 2 + 1];
        v.last = (n + 1 == nsamp) ? 1 : 0;
        in.write(v);
        // The core consumes at most one sample per call and may emit none.
        qpsk_demod_top(in, out, enable, locks);
        while (!out.empty()) got.push_back((uint8_t)out.read().data);
    }

    Match m = bestAlignment(got, want);
    const double pct = m.compared ? 100.0 * double(m.matched) / double(m.compared) : 0.0;
    std::printf("  %-10s in=%zu samples  out=%zu bytes  best_run=%zu  match=%.1f%% "
                "at offset %zu  locks=%u\n",
                name.c_str(), nsamp, got.size(), m.run, pct, m.offset,
                // Copy out of the volatile before converting -- same rule that
                // bit the design itself: ap_uint has a volatile-aware copy
                // constructor but no volatile conversion operators.
                (unsigned)ap_uint<32>(locks).to_uint());

    if (got.empty()) { std::printf("             FAIL: no output at all\n"); return 1; }
    // Bit-exact requirement.
    //
    // The matched filter needs NTAPS/sps symbols of history before its output
    // is meaningful, so the first few bytes are fill and cannot match. Allow
    // exactly that much and require every remaining byte to be correct --
    // "most of them" is not a correctness gate.
    const size_t kFill = 4;                       // ~49/4 symbols = 3 bytes, +1
    const size_t need = (want.size() > kFill) ? want.size() - kFill : want.size();
    if (m.run < need) {
        std::printf("             FAIL: longest exact run %zu < required %zu\n", m.run, need);
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
    for (const char* v : {"clean", "impaired"}) {
        if (runVector(dir, v) != 0) rc = 1;
    }
    std::printf("%s\n", rc == 0 ? "csim: PASS" : "csim: FAIL");
    return rc;
}
