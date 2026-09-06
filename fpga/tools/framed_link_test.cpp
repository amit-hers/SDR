// framed_link_test -- continuous framed end-to-end measurement over RF.
//
//   build:  g++ -O2 -std=c++17 -I include -o framed_link_test \
//               fpga/tools/framed_link_test.cpp build/src/core/libsdr_core.a -lliquid -lcrypto
//
//   gen <n> <out.iq> <out.bytes>       numbered frames -> int16 IQ for iio_writedev
//   acq <cap...>                       cold-start acquisition, one file per trial
//   ser <cap> <ref.bytes> <txlen>      channel byte/symbol error rate
//   per <cap> <ref.bytes> <txlen>      per-frame loss, scoped to whole frames
//
// Transmit the .iq with fpga/scripts/tx_cyclic.sh and receive with
// fpga/scripts/rx_framed.sh; <txlen> is the byte-stream length that actually
// fits the cyclic DAC buffer, which is what the receiver sees repeating.
//
//   gen  : build a long run of NUMBERED frames, modulate them exactly as
//          qpsk_mod.cpp would, and write int16 IQ for iio_writedev.
//   rx   : take the demodulated byte stream captured off the RX board and run
//          it through FOUR Deframers, one per symbol phase.
//
// The four instances are not redundancy. The demodulator packs four 2-bit
// symbols per byte starting at an arbitrary symbol, so the byte grid it emits
// is offset from the transmitted one by 0, 1, 2 or 3 symbols -- 0, 2, 4 or 6
// bits. A single Deframer sees the sync word split across two bytes in three
// of the four cases and never matches. Nothing in the stream says which case
// it is, so all four run and whichever locks is the alignment.
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <algorithm>

using namespace sdr;

// ── The fabric modulator, reproduced ──────────────────────────────────────
// Taps and constants are copied from fpga/hls/qpsk_modem/qpsk_common.h; the
// symbol map, the differential rule and the polyphase indexing are copied from
// qpsk_mod.cpp. Anything that drifts from those files puts a waveform on the
// air that the core was not built to receive, so they are transcribed rather
// than approximated.
static const float RRC_H[49] = {
    -0.000967f,-0.000741f, 0.000454f, 0.001516f, 0.001238f,-0.000392f,
    -0.001919f,-0.001695f, 0.000337f, 0.002198f, 0.001580f,-0.001576f,
    -0.004200f,-0.002438f, 0.004227f, 0.010783f, 0.009426f,-0.003642f,
    -0.022304f,-0.031127f,-0.013975f, 0.034137f, 0.100291f, 0.157940f,
     0.180795f, 0.157940f, 0.100291f, 0.034137f,-0.013975f,-0.031127f,
    -0.022304f,-0.003642f, 0.009426f, 0.010783f, 0.004227f,-0.002438f,
    -0.004200f,-0.001576f, 0.001580f, 0.002198f, 0.000337f,-0.001695f,
    -0.001919f,-0.000392f, 0.001238f, 0.001516f, 0.000454f,-0.000741f,
    -0.000967f
};
static const int NTAPS = 49, SPS = 4, TAPS_PER_PHASE = 13;

// fixp_t is ap_fixed<16,1>: 15 fractional bits, and ap_fixed truncates toward
// -inf rather than rounding. Quantising the same way keeps the host waveform
// bit-comparable with the core's own output instead of merely close.
static inline float q15(float v) { return std::floor(v * 32768.0f) / 32768.0f; }

static void modulate(const std::vector<uint8_t>& bytes, bool diff,
                     std::vector<int16_t>& iq)
{
    float hq[49];
    for (int k = 0; k < NTAPS; ++k) hq[k] = q15(RRC_H[k]);
    const float A_POS = q15( 0.707f), A_NEG = q15(-0.707f);

    float di[TAPS_PER_PHASE] = {0}, dq[TAPS_PER_PHASE] = {0};
    uint8_t phase = 0;
    iq.clear();
    iq.reserve(bytes.size() * 4 * SPS * 2);

    for (uint8_t b : bytes) {
        for (int s = 0; s < 4; ++s) {
            uint8_t bits = (b >> (6 - s * 2)) & 0x3;
            if (diff) {
                // Accumulate in PHASE and convert bin->Gray, never in the
                // symbol index: the index walks the circle 0,1,3,2, so a
                // 90-degree rotation is not a constant offset in it and the
                // difference does not cancel it. Same rule as qpsk_mod.cpp.
                phase = (uint8_t)((phase + bits) & 3);
                bits  = (uint8_t)(phase ^ (phase >> 1));
            }
            // bit0 steers I, bit1 steers Q -- liquid's LIQUID_MODEM_QPSK.
            float i_sym = (bits & 1) ? A_NEG : A_POS;
            float q_sym = (bits & 2) ? A_NEG : A_POS;

            for (int k = TAPS_PER_PHASE - 1; k > 0; --k) {
                di[k] = di[k-1]; dq[k] = dq[k-1];
            }
            di[0] = i_sym; dq[0] = q_sym;

            for (int k = 0; k < SPS; ++k) {
                float ai = 0, aq = 0;
                for (int t = 0; t < TAPS_PER_PHASE; ++t) {
                    int ti = t * SPS + k;
                    if (ti < NTAPS) { ai += di[t] * hq[ti]; aq += dq[t] * hq[ti]; }
                }
                const float LIM = 0.999f;
                ai = std::max(-LIM, std::min(LIM, ai));
                aq = std::max(-LIM, std::min(LIM, aq));
                iq.push_back((int16_t)std::lrint(ai * 32768.0f));
                iq.push_back((int16_t)std::lrint(aq * 32768.0f));
            }
        }
    }
}

// Payload is a seq-derived PRBS so a frame that passes CRC can still be
// checked against what was actually sent -- a CRC agreeing with a corrupted
// header would otherwise go unnoticed.
static void fillPayload(std::vector<uint8_t>& p, uint32_t seq, bool zeros) {
    if (zeros) { std::fill(p.begin(), p.end(), 0); return; }
    uint32_t x = seq * 2654435761u + 1u;
    for (auto& b : p) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = (uint8_t)(x >> 24); }
}

// Four frame VARIANTS, cycling by sequence number, in one continuous stream.
//
// Running one payload size per capture would need four separate uploads and
// four different minutes of channel, and PER is steep enough in frame length
// that comparing across them would be comparing weather as much as coding.
// Interleaving the variants puts all four on the same air, symbol for symbol.
// `zeros` fills the payload with 0x00 instead of a PRBS. That is not a corner
// case invented for the test: differential QPSK turns a run of 0x00 into a run
// of IDENTICAL symbols -- phase increments of zero -- so the constellation
// stops moving and the Gardner detector, which needs transitions, has nothing
// to measure. H.264 is full of long zero runs, so whether the loop survives one
// decides whether video works at all. Reed-Solomon reaches the same place by a
// different road: it zero-pads a short payload out to 223 bytes, which is why
// the 32-byte RS variant is here alongside the 200-byte one.
struct Variant { size_t paylen; bool fec; bool zeros; };
// Payload-length sweep at realistic video frame sizes. Every wire length stays
// under the DMA's 1024-byte packet so that a frame can be wholly contained in
// one capture and its loss attributed to the link rather than to the boundary.
static const Variant VARIANTS[4] = {
    {  64, false, false },   // wire 134
    { 256, false, false },   // wire 326
    { 512, false, false },   // wire 582
    { 700, false, false },   // wire 770
};
static const int NVAR = 4;

static size_t wireLen(const Variant& v) {
    size_t body = v.fec ? ReedSolomon::encodedSize(v.paylen) : v.paylen;
    return PREAMBLE_LEN + FRAME_OVERHEAD + POSTAMBLE_LEN + body;
}

static int doGen(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "gen <nframes> <out.iq> <out.bytes>\n"); return 2; }
    int      nfr    = atoi(argv[2]);
    ReedSolomon rs;
    Framer fr;

    std::vector<uint8_t> stream;
    size_t cycle = 0;
    for (int v = 0; v < NVAR; ++v) cycle += wireLen(VARIANTS[v]);

    for (int n = 0; n < nfr; ++n) {
        const Variant& v = VARIANTS[n % NVAR];
        std::vector<uint8_t> pay(v.paylen);
        fillPayload(pay, (uint32_t)n, v.zeros);
        auto f = fr.encode(pay.data(), pay.size(), v.fec ? FL_FEC : 0,
                           ModCode::QPSK, BwCode::BW_5, 0x5D0A, (uint32_t)n,
                           v.fec ? &rs : nullptr, nullptr);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    std::vector<int16_t> iq;
    modulate(stream, /*diff=*/true, iq);

    FILE* f1 = fopen(argv[3], "wb");
    fwrite(iq.data(), 2, iq.size(), f1); fclose(f1);
    FILE* f2 = fopen(argv[4], "wb");
    fwrite(stream.data(), 1, stream.size(), f2); fclose(f2);

    printf("frames=%d  cycle=%zu B  stream=%zu B  iq=%zu samples (%.1f MB)\n",
           nfr, cycle, stream.size(), iq.size()/2, iq.size()*2.0/1e6);
    for (int v = 0; v < NVAR; ++v)
        printf("  variant %d: paylen %3zu  fec %d  fill %-6s  wire %4zu B\n",
               v, VARIANTS[v].paylen, (int)VARIANTS[v].fec,
               VARIANTS[v].zeros ? "zeros" : "random", wireLen(VARIANTS[v]));
    printf("loop period at 7.68 MS/s = %.3f s\n", (iq.size()/2) / 7.68e6);
    return 0;
}

// ── RX ────────────────────────────────────────────────────────────────────
struct Hit { size_t pos; uint32_t seq; bool payload_ok; };

// Shift the whole capture left by `bits` so the transmitted byte grid is
// restored. `bits` is always a multiple of 2: symbols are atomic, so only four
// alignments are possible and one of them is the transmitter's.
static std::vector<uint8_t> realign(const std::vector<uint8_t>& in, int bits) {
    std::vector<uint8_t> out(in.size() ? in.size() - 1 : 0);
    for (size_t n = 0; n + 1 < in.size(); ++n)
        out[n] = (uint8_t)((in[n] << bits) | (in[n+1] >> (8 - bits)));
    return out;
}

static int doRx(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "rx <cap.bin> [byterate]\n"); return 2; }
    FILE* f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    std::vector<uint8_t> cap;
    { uint8_t buf[65536]; size_t n; while ((n = fread(buf,1,sizeof buf,f))) cap.insert(cap.end(), buf, buf+n); }
    fclose(f);
    double brate = argc > 3 ? atof(argv[3]) : 480000.0;   // demod bytes/s
    ReedSolomon rs;

    size_t cycle = 0;
    for (int v = 0; v < NVAR; ++v) cycle += wireLen(VARIANTS[v]);

    printf("capture %zu B  = %.3f s of air  = %.0f frame cycles (%zu B each)\n\n",
           cap.size(), cap.size()/brate, cap.size()/(double)cycle, cycle);

    // FOUR DEFRAMERS IN PARALLEL, and their results MERGED -- not a search for
    // the one true offset.
    //
    // Taking only the best-scoring offset threw away 70% of the frames that
    // were actually recovered (471 of 1567 in the first run). The byte grid is
    // not fixed for the length of a capture: the DMA delivers 1024-byte
    // packets with gaps between them, the demodulator keeps emitting through
    // each gap, and the number of bytes lost there is arbitrary -- so the
    // symbol phase of one packet says nothing about the next. A real receiver
    // runs all four and accepts whichever produces a valid CRC, which is what
    // this now measures.
    std::vector<Hit> all; uint64_t crcerr = 0, rescued = 0;
    int per_off[4] = {0,0,0,0};

    for (int off = 0; off < 4; ++off) {
        std::vector<uint8_t> s2 = off ? realign(cap, off*2) : cap;
        Deframer d; size_t got = 0;
        for (size_t n = 0; n < s2.size(); ++n) {
            auto r = d.push(s2[n], &rs, nullptr);
            if (r) {
                const Variant& v = VARIANTS[r->seq % NVAR];
                std::vector<uint8_t> pay(v.paylen);
                fillPayload(pay, r->seq, v.zeros);
                bool ok = r->payload.size() == v.paylen &&
                          std::memcmp(pay.data(), r->payload.data(), v.paylen) == 0;
                all.push_back({n + (size_t)(off?1:0), r->seq, ok});
                ++got;
            }
        }
        per_off[off] = (int)got;
        crcerr += d.crcErrors(); rescued += d.fecRescued();
        printf("  offset %d bits: %5zu CRC-valid frames, %6llu CRC failures\n",
               off*2, got, (unsigned long long)d.crcErrors());
    }
    std::sort(all.begin(), all.end(),
              [](const Hit& a, const Hit& b){ return a.pos < b.pos; });
    // A frame can only be decoded once; two offsets reporting the same seq at
    // nearly the same byte are the same frame seen twice, not two receptions.
    std::vector<Hit> best;
    for (auto& h : all) {
        bool dup = false;
        for (auto it = best.rbegin(); it != best.rend() && h.pos - it->pos < 8; ++it)
            if (it->seq == h.seq) { dup = true; break; }
        if (!dup) best.push_back(h);
    }
    uint64_t best_crcerr = crcerr;
    printf("  merged (dedup):%5zu CRC-valid frames\n", best.size());

    // Acquisition. The first recovered frame ENDS at best[0].pos, so the link
    // was already good by the time its own wire length started -- that is the
    // latest instant acquisition can have completed, hence an upper bound.
    const Variant& v0 = VARIANTS[best[0].seq % NVAR];
    double acq = (double)best[0].pos - (double)wireLen(v0);
    if (acq < 0) acq = 0;
    printf("first valid frame: seq %u ends at byte %zu -> acquisition <= %.0f B = %.2f ms\n",
           best[0].seq, best[0].pos, acq, acq / brate * 1000.0);

    // Per-variant PER. One frame of each variant occurs per cycle, so the
    // number that SHOULD have arrived between the first and last recovery is
    // just the byte span divided by the cycle length -- no need to guess at
    // which slots were which.
    size_t span = best.back().pos - best[0].pos;
    double slots_per_variant = span / (double)cycle;
    printf("\nspan %zu B (%.3f s), %.0f cycles -> %.0f slots per variant\n",
           span, span/brate, slots_per_variant, slots_per_variant);
    printf("\n  variant  paylen  fec  wire   recovered   slots     PER      payload_bad\n");
    size_t total_bad = 0;
    for (int v = 0; v < NVAR; ++v) {
        size_t got = 0, bad = 0;
        for (auto& h : best) if ((int)(h.seq % NVAR) == v) { ++got; if (!h.payload_ok) ++bad; }
        total_bad += bad;
        double per = slots_per_variant > 0 ? 1.0 - got/slots_per_variant : 0;
        if (per < 0) per = 0;
        printf("  %7d  %6zu  %3d  %4zu   %9zu   %5.0f   %6.2f%%   %10zu\n",
               v, VARIANTS[v].paylen, (int)VARIANTS[v].fec, wireLen(VARIANTS[v]),
               got, slots_per_variant, per*100.0, bad);
    }

    // Sequence continuity across everything that did arrive.
    size_t gaps = 0, missing = 0, wraps = 0;
    uint32_t prev = best[0].seq;
    for (size_t k = 1; k < best.size(); ++k) {
        uint32_t s3 = best[k].seq;
        if (s3 < prev) { ++wraps; prev = s3; continue; }
        uint32_t d = s3 - prev;
        if (d != 1) { ++gaps; missing += (d - 1); }
        prev = s3;
    }
    printf("\ntotal: %zu CRC-valid frames, %llu CRC failures, %llu rescued by Reed-Solomon\n",
           best.size(), (unsigned long long)best_crcerr, (unsigned long long)rescued);
    printf("sequence: %zu wraps, %zu gaps, %zu missing seq numbers\n", wraps, gaps, missing);
    printf("payload mismatches among CRC-valid frames: %zu\n", total_bad);

    size_t good_bytes = 0;
    for (auto& h : best) good_bytes += VARIANTS[h.seq % NVAR].paylen;
    printf("goodput: %.3f Mbit/s over the whole capture\n",
           good_bytes*8.0 / (cap.size()/brate) / 1e6);
    return 0;
}


// Anchor a packet that decoded NOTHING, by predicting from the previous one.
//
// Skipping such packets silently conditions every rate on "the packet was good
// enough to decode at least one frame", which is the very thing being measured.
// The DMA re-arms after a fixed 1024 bytes and the measured gap is a couple of
// bytes, so the next packet's position in the transmitted stream is known to
// within a few tens of bytes -- near enough to search directly.
static bool predictAnchor(const std::vector<uint8_t>& win,
                          const std::vector<uint8_t>& ref,
                          long predicted, long& shift_out, int& off_out)
{
    const long RL = (long)ref.size();
    long best_err = -1;
    for (int off = 0; off < 4; ++off) {
        std::vector<uint8_t> s2 = off ? realign(win, off*2) : win;
        for (long cand = predicted - 64; cand <= predicted + 64; ++cand) {
            long err = 0;
            for (size_t k = 0; k < s2.size(); ++k) {
                long ri = (cand + (long)k) % RL; if (ri < 0) ri += RL;
                if (s2[k] != ref[(size_t)ri]) ++err;
            }
            if (best_err < 0 || err < best_err) { best_err = err; shift_out = cand; off_out = off; }
        }
    }
    // Half the bytes wrong is chance for a stream this random; demand clearly
    // better than that before believing the alignment.
    return best_err >= 0 && best_err < (long)(win.size() * 0.45);
}

// Byte-error rate, anchored on frames that decoded.
//
// A decoded frame is an exact position fix: its sequence number says where in
// the transmitted stream its last byte sits, so the whole DMA packet around it
// can be compared against what was actually sent. That measures the CHANNEL
// without the framing on top, which is the only way to tell a link that is
// dropping frames to bit errors from one dropping them to capture gaps.
// Restricted to the packet containing the anchor: the byte grid says nothing
// across a packet boundary, because the demodulator keeps emitting through the
// gap and an unknown number of bytes never reach the buffer.
static int doSer(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "ser <cap.bin> <ref.bytes> <txlen>\n"); return 2; }
    auto slurp = [](const char* fn) {
        std::vector<uint8_t> v; FILE* f = fopen(fn, "rb");
        if (!f) { perror(fn); exit(1); }
        uint8_t b[65536]; size_t n;
        while ((n = fread(b,1,sizeof b,f))) v.insert(v.end(), b, b+n);
        fclose(f); return v;
    };
    std::vector<uint8_t> cap = slurp(argv[2]), ref = slurp(argv[3]);
    size_t txlen = (size_t)atol(argv[4]);
    if (txlen && txlen < ref.size()) ref.resize(txlen);
    ReedSolomon rs;

    // Byte offset of each frame within the transmitted stream.
    std::vector<size_t> startOf; size_t acc = 0;
    for (int n = 0; n < 100000; ++n) { startOf.push_back(acc); acc += wireLen(VARIANTS[n % NVAR]); }
    (void)0;

    const size_t PKT = 1024;
    size_t tot_bytes = 0, tot_err = 0, pkts = 0, anchored = 0;
    std::vector<double> per_pkt;

    for (size_t pk = 0; pk + PKT <= cap.size(); pk += PKT) {
        ++pkts;
        // Each offset is anchored and scored SEPARATELY, and the packet takes
        // the best of the four.
        //
        // Scoring the whole packet at whichever offset happened to decode first
        // charges every byte after a symbol slip as an error, which reported
        // 4.36% where the framing was simultaneously losing 0.07% of 270-byte
        // frames -- two numbers that cannot both describe the same bytes. The
        // slip is real, but it is a change of alignment, not corruption.
        double bestv = -1; size_t best_err = 0, best_cnt = 0;
        for (int off = 0; off < 4; ++off) {
            std::vector<uint8_t> win(cap.begin()+pk, cap.begin()+pk+PKT);
            std::vector<uint8_t> s2 = off ? realign(win, off*2) : win;
            Deframer d; bool got = false; long shift = 0;
            for (size_t n = 0; n < s2.size() && !got; ++n) {
                auto r = d.push(s2[n], &rs, nullptr);
                if (!r) continue;
                size_t refend = (startOf[r->seq] + wireLen(VARIANTS[r->seq % NVAR])
                                 - POSTAMBLE_LEN) % ref.size();
                shift = (long)refend - 1 - (long)n; got = true;
            }
            if (!got) continue;
            size_t err = 0;
            for (size_t k = 0; k < s2.size(); ++k) {
                long ri = (shift + (long)k) % (long)ref.size();
                if (ri < 0) ri += (long)ref.size();
                if (s2[k] != ref[(size_t)ri]) ++err;
            }
            double v = (double)err / s2.size();
            if (bestv < 0 || v < bestv) { bestv = v; best_err = err; best_cnt = s2.size(); }
        }
        if (bestv < 0) continue;
        tot_bytes += best_cnt; tot_err += best_err; ++anchored;
        per_pkt.push_back(100.0 * bestv);
    }
    if (!anchored) { printf("no packet could be anchored\n"); return 0; }
    std::sort(per_pkt.begin(), per_pkt.end());
    double byte_er = (double)tot_err / (double)tot_bytes;
    // Four symbols to a byte, so a byte survives only if all four do.
    double ser = 1.0 - std::pow(1.0 - byte_er, 0.25);
    printf("anchored %zu of %zu packets (%.1f%%)\n", anchored, pkts, 100.0*anchored/pkts);
    printf("byte error rate %.4f%%   -> symbol error rate %.4f%%\n", byte_er*100, ser*100);
    printf("per-packet byte error: median %.4f%%  p90 %.4f%%  worst %.4f%%\n",
           per_pkt[per_pkt.size()/2], per_pkt[(size_t)(per_pkt.size()*0.9)], per_pkt.back());
    size_t clean = 0; for (double v : per_pkt) if (v == 0.0) ++clean;
    printf("packets with ZERO byte errors: %zu of %zu (%.1f%%)\n",
           clean, per_pkt.size(), 100.0*clean/per_pkt.size());
    return 0;
}

// Packet-scoped PER: the honest frame-loss number.
//
// A frame that straddles a DMA packet boundary was never fully captured, so
// counting it as a link loss measures the capture method rather than the
// radio. Anchoring each 1024-byte packet against the transmitted stream says
// exactly which frames were WHOLLY PRESENT in it; those, and only those, had a
// chance to decode. The difference is not small -- it is most of the loss.
static int doPer(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "per <cap.bin> <ref.bytes> <txlen>\n"); return 2; }
    auto slurp = [](const char* fn) {
        std::vector<uint8_t> v; FILE* f = fopen(fn, "rb");
        if (!f) { perror(fn); exit(1); }
        uint8_t b[65536]; size_t n;
        while ((n = fread(b,1,sizeof b,f))) v.insert(v.end(), b, b+n);
        fclose(f); return v;
    };
    std::vector<uint8_t> cap = slurp(argv[2]), ref = slurp(argv[3]);
    size_t txlen = (size_t)atol(argv[4]);
    if (txlen && txlen < ref.size()) ref.resize(txlen);
    const size_t RL = ref.size();
    ReedSolomon rs;

    // Frames that START inside one period of the cyclic transmit buffer, and
    // no more. Enumerating two periods counts every frame twice, which inflates
    // the denominator and halves the apparent success rate.
    std::vector<size_t> startOf; size_t acc = 0; int nseq = 0;
    while (acc + wireLen(VARIANTS[nseq % NVAR]) <= RL) {
        startOf.push_back(acc); acc += wireLen(VARIANTS[nseq % NVAR]); ++nseq;
    }

    const size_t PKT = 1024;
    size_t capturable[NVAR] = {0}, decoded[NVAR] = {0}, badpay = 0;
    size_t pkts = 0, anchored = 0, rescued = 0, crcfail = 0;
    std::vector<long> gaps; long prev_shift = 0; bool have_prev = false;
    size_t predicted_anchor = 0, unanchored = 0;

    for (size_t pk = 0; pk + PKT <= cap.size(); pk += PKT) {
        ++pkts;
        std::vector<uint8_t> win(cap.begin()+pk, cap.begin()+pk+PKT);
        std::map<uint32_t,size_t> got;          // seq -> index of last byte
        long shift = 0; bool have = false;
        for (int off = 0; off < 4; ++off) {
            std::vector<uint8_t> s2 = off ? realign(win, off*2) : win;
            Deframer d;
            for (size_t n = 0; n < s2.size(); ++n) {
                auto r = d.push(s2[n], &rs, nullptr);
                if (!r) continue;
                const Variant& v = VARIANTS[r->seq % NVAR];
                std::vector<uint8_t> pay(v.paylen);
                fillPayload(pay, r->seq, v.zeros);
                if (r->payload.size() != v.paylen ||
                    std::memcmp(pay.data(), r->payload.data(), v.paylen) != 0) ++badpay;
                if (!got.count(r->seq)) got[r->seq] = n;
                if (!have) {
                    size_t refend = (startOf[r->seq] + wireLen(v) - POSTAMBLE_LEN) % RL;
                    shift = (long)refend - 1 - (long)n; have = true;
                }
            }
            rescued += d.fecRescued(); crcfail += d.crcErrors();
        }
        if (!have && have_prev) {
            int dummy;
            if (predictAnchor(win, ref, prev_shift + (long)PKT, shift, dummy)) have = true;
            if (have) ++predicted_anchor;
        }
        if (!have) { have_prev = false; ++unanchored; continue; }
        ++anchored;
        // Bytes the demodulator emitted between this packet and the last one
        // that never reached the buffer. The DMA is re-armed after every
        // 1024-byte transfer and the core does not stop for it.
        if (have_prev) {
            long g = shift - prev_shift - (long)PKT;
            while (g < -(long)RL/2) g += (long)RL;
            while (g >  (long)RL/2) g -= (long)RL;
            if (g >= 0) gaps.push_back(g);
        }
        prev_shift = shift; have_prev = true;
        // Reference span this packet covers, as a half-open interval that may
        // wrap the cyclic transmit buffer.
        long lo = shift, hi = shift + (long)PKT;
        for (int q = 0; q < nseq; ++q) {
            long a = (long)startOf[q];
            long b = a + (long)wireLen(VARIANTS[q % NVAR]) - (long)POSTAMBLE_LEN;
            for (long rot = -(long)RL; rot <= (long)RL; rot += (long)RL) {
                if (a + rot >= lo && b + rot <= hi) {
                    int v = q % NVAR;
                    ++capturable[v];
                    if (got.count((uint32_t)q)) ++decoded[v];
                }
            }
        }
    }

    printf("packets %zu, anchored %zu (%.1f%%)  -- %zu of them by prediction "
           "because no frame in them decoded; %zu could not be placed at all\n"
           "reference stream %zu B\n\n",
           pkts, anchored, 100.0*anchored/pkts, predicted_anchor, unanchored, RL);
    printf("  variant  paylen  fec  fill    wire   capturable  decoded      PER\n");
    size_t tc = 0, td = 0;
    for (int v = 0; v < NVAR; ++v) {
        tc += capturable[v]; td += decoded[v];
        double per = capturable[v] ? 1.0 - (double)decoded[v]/capturable[v] : 0;
        printf("  %7d  %6zu  %3d  %-6s  %4zu   %10zu  %7zu   %6.2f%%\n",
               v, VARIANTS[v].paylen, (int)VARIANTS[v].fec,
               VARIANTS[v].zeros ? "zeros" : "random", wireLen(VARIANTS[v]),
               capturable[v], decoded[v], per*100.0);
    }
    printf("\ntotal capturable %zu, decoded %zu, MISSING %zu -> PER %.2f%%\n",
           tc, td, tc - td, tc ? (1.0-(double)td/tc)*100.0 : 0.0);
    printf("CRC failures %zu, Reed-Solomon rescues %zu, payload mismatches %zu\n",
           crcfail, rescued, badpay);
    if (!gaps.empty()) {
        std::sort(gaps.begin(), gaps.end());
        double mean = 0; for (long g : gaps) mean += g; mean /= gaps.size();
        printf("\nDMA gap between consecutive packets: median %ld B, mean %.0f B, p90 %ld B, max %ld B\n",
               gaps[gaps.size()/2], mean, gaps[(size_t)(gaps.size()*0.9)], gaps.back());
        printf("capture duty cycle: %.1f%% of the demodulated stream reached userspace\n",
               100.0 * PKT / (PKT + mean));
    }
    return 0;
}

// Cold-start acquisition: how far into the FIRST captured packet the receiver
// had to get before a frame decoded. Only the first packet counts -- every
// later one begins after a DMA re-arm, which is a warm restart, not a cold one.
static int doAcq(int argc, char** argv) {
    double brate = 480000.0;
    printf("  trial   first frame   sync starts at   acquisition\n");
    std::vector<double> acq;
    ReedSolomon rs;
    for (int a = 2; a < argc; ++a) {
        FILE* f = fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> cap(1024);
        size_t n = fread(cap.data(), 1, 1024, f); fclose(f);
        cap.resize(n);
        long bestpos = -1; uint32_t bestseq = 0;
        for (int off = 0; off < 4; ++off) {
            std::vector<uint8_t> s2 = off ? realign(cap, off*2) : cap;
            Deframer d;
            for (size_t k = 0; k < s2.size(); ++k) {
                auto r = d.push(s2[k], &rs, nullptr);
                if (!r) continue;
                const Variant& v = VARIANTS[r->seq % NVAR];
                size_t body = v.fec ? ReedSolomon::encodedSize(v.paylen) : v.paylen;
                long sync = (long)k - (long)(HEADER_SIZE + body + CRC_SIZE) + 1;
                if (bestpos < 0 || sync < bestpos) { bestpos = sync; bestseq = r->seq; }
                break;
            }
        }
        if (bestpos < 0) { printf("  %5d   %11s\n", a-1, "none"); continue; }
        if (bestpos < 0) bestpos = 0;
        acq.push_back((double)bestpos);
        printf("  %5d   seq %7u   %10ld B      %6.2f ms\n",
               a-1, bestseq, bestpos, bestpos / brate * 1000.0);
    }
    if (acq.empty()) { printf("\nno trial acquired\n"); return 0; }
    std::sort(acq.begin(), acq.end());
    double mean = 0; for (double v : acq) mean += v; mean /= acq.size();
    printf("\nacquired in %zu of %d trials\n", acq.size(), argc-2);
    printf("acquisition bytes: median %.0f  mean %.0f  p90 %.0f  worst %.0f\n",
           acq[acq.size()/2], mean, acq[(size_t)(acq.size()*0.9)], acq.back());
    printf("acquisition time : median %.2f ms  mean %.2f ms  worst %.2f ms\n",
           acq[acq.size()/2]/brate*1000, mean/brate*1000, acq.back()/brate*1000);
    printf("acquisition symbols: median %.0f  worst %.0f\n",
           acq[acq.size()/2]*4, acq.back()*4);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: framedtest gen|rx ...\n"); return 2; }
    if (!strcmp(argv[1], "gen")) return doGen(argc, argv);
    if (!strcmp(argv[1], "rx"))  return doRx(argc, argv);
    if (!strcmp(argv[1], "ser")) return doSer(argc, argv);
    if (!strcmp(argv[1], "per")) return doPer(argc, argv);
    if (!strcmp(argv[1], "acq")) return doAcq(argc, argv);
    fprintf(stderr, "unknown mode %s\n", argv[1]); return 2;
}
