// Can the Pluto's ARM keep up with the bridge's framing path?
//
// The receive path decodes FOUR symbol phases over every byte, so it does four
// times the work the byte rate suggests. If it cannot sustain the link rate the
// reads fall behind, the DMA overflows, and the result presents as packet loss
// on the air -- a CPU limit wearing an RF fault's clothes, which is exactly the
// kind of misattribution this project has paid for repeatedly.
//
// Reports MB/s of DEMODULATED INPUT consumed, which is the quantity to compare
// against the link: at 17.28 MS/s with 4 samples per symbol and 4 symbols per
// byte, the demodulator emits 1.08 MB/s.
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/OffsetDeframer.hpp"
#include "sdr/framing/Frame.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace sdr;

int main(int argc, char** argv) {
    const int    npkt = argc > 1 ? std::atoi(argv[1]) : 60;
    const size_t pkt  = argc > 2 ? static_cast<size_t>(std::atol(argv[2])) : 32768;
    const size_t paylen = 1200;

    // Build one packet's worth of real frames, scrambled and CRC'd exactly as
    // the transmitter produces them -- a synthetic byte pattern would not
    // exercise the same branches in the sync hunt.
    Framer framer;
    std::vector<uint8_t> stream;
    uint32_t seq = 0;
    std::vector<uint8_t> pay(paylen);
    for (size_t i = 0; i < paylen; ++i) pay[i] = static_cast<uint8_t>(i * 31 + 7);
    while (stream.size() < pkt) {
        auto w = framer.encode(pay, 0, ModCode::QPSK, BwCode::BW_5, 1, seq++, nullptr, nullptr);
        stream.insert(stream.end(), w.begin(), w.end());
    }
    stream.resize(pkt);

    std::printf("packet %zu B, %d packets, %zu B payloads\n", pkt, npkt, paylen);

    // ── TX: framing throughput ───────────────────────────────────────────
    {
        auto t0 = std::chrono::steady_clock::now();
        size_t bytes = 0;
        for (int i = 0; i < npkt; ++i) {
            for (size_t off = 0; off + paylen <= pkt; off += paylen) {
                auto w = framer.encode(pay, 0, ModCode::QPSK, BwCode::BW_5, 1, seq++, nullptr, nullptr);
                bytes += w.size();
            }
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  TX framing   : %7.2f MB/s of wire produced\n", bytes / s / 1e6);
    }

    // ── RX: four-offset decode throughput ────────────────────────────────
    {
        OffsetDeframer d;
        auto t0 = std::chrono::steady_clock::now();
        size_t bytes = 0, frames = 0;
        for (int i = 0; i < npkt; ++i) {
            auto got = d.pushPacket(stream.data(), stream.size());
            frames += got.size();
            bytes  += stream.size();
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double mbps = bytes / s / 1e6;
        std::printf("  RX 4-offset  : %7.2f MB/s of demodulated input consumed\n", mbps);
        // Every iteration decodes the SAME stream, so only the first packet's
        // frames are new -- the rest are deduplicated by design. The decode
        // work is still done in full each time, so the rate above is valid;
        // this count is per-packet, not a total.
        std::printf("  (%zu frames from the first packet, %llu CRC failures;\n"
                    "   later packets repeat those seqs and are deduplicated)\n",
                    frames, (unsigned long long)d.crcErrors());
        std::printf("\n");
        // Compare against the two operating points that matter.
        struct { const char* name; double ms; } pts[] = {
            {"3.84 MS/s", 3.84e6}, {"7.68 MS/s", 7.68e6}, {"17.28 MS/s", 17.28e6},
        };
        for (auto& p : pts) {
            double need = p.ms / 4.0 / 4.0 / 1e6;   // sps=4, 4 symbols per byte
            std::printf("  %-11s needs %5.3f MB/s -> %s (%.1fx margin)\n",
                        p.name, need, mbps > need ? "OK" : "TOO SLOW", mbps / need);
        }
    }
    return 0;
}
