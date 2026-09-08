// Emit the project's wire formats as raw bytes, so the SAME source built for a
// different architecture can be diffed against them.
//
// This exists because the two ends of the link will not be the same machine.
// The daemon runs on x86; the bridge is destined for the Pluto's ARMv7. A wire
// format that silently differs between them fails as "the radio does not
// decode", which is indistinguishable from an RF fault and would be chased as
// one. StatePacket and Framer both write fields by hand precisely so this
// cannot happen -- and this proves it rather than assuming it.
//
// No Reed-Solomon and no AES: those pull in liquid-dsp and OpenSSL, which is a
// packaging problem, not a wire-format one. The bytes tested here are the ones
// that carry the endianness and width assumptions.
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Scrambler.hpp"
#include "sdr/telemetry/StatePacket.hpp"
#include <cstdio>
#include <vector>

using namespace sdr;

static void dump(const char* tag, const std::vector<uint8_t>& v) {
    std::printf("%s %zu ", tag, v.size());
    for (uint8_t b : v) std::printf("%02x", b);
    std::printf("\n");
}

int main() {
    // Sizes and signedness of everything the wire depends on.
    std::printf("sizes %zu %zu %zu %zu %zu\n",
                sizeof(int), sizeof(long), sizeof(size_t),
                sizeof(float), sizeof(double));
    std::printf("charsigned %d\n", (int)(char(-1) < 0));

    // A state report with negative fields, large counters and every width in use.
    StatePacket s;
    s.node_id = 0xDEADBEEF; s.uptime_s = 0x01020304; s.mode = 3;
    s.freq_tx_hz = 434000000u; s.freq_rx_hz = 439000000u; s.bw_hz = 1000000u;
    s.sps = 4; s.modulation = 2; s.tx_atten_cdb = -1234;
    s.frames_tx = 0x0102030405060708ull; s.frames_rx_good = 0xFFFFFFFFFFFFFFFFull;
    s.frames_rx_bad = 7; s.dropped = 3;
    s.bytes_tx = 1234567890123ull; s.bytes_rx = 42; s.fec_corrected = 5;
    s.rssi_cdbm = -6350; s.snr_cdb = 1875;
    s.tx_kbps = 65535; s.rx_kbps = 1; s.tx_duty_pct = 99; s.temp_cc = -500;
    s.fpga_magic = 0x5344524C; s.fpga_version = 0x00010300;
    s.fpga_abi = 3; s.regmap_ver = 3;
    dump("statepacket", s.encode());

    // A frame, which exercises the scrambler and the CRC as well as the header.
    Framer fr;
    std::vector<uint8_t> pay(64);
    for (size_t i = 0; i < pay.size(); ++i) pay[i] = uint8_t(i * 7 + 3);
    dump("frame", fr.encode(pay.data(), pay.size(), 0, ModCode::QPSK,
                            BwCode::BW_10, 0xCAFEBABE, 0x11223344, nullptr, nullptr));

    // The scrambler alone, on a run of zeros -- the case it exists for.
    std::vector<uint8_t> z(48, 0);
    Scrambler::apply(z.data(), z.size(), 99);
    dump("scrambler", z);
    return 0;
}
