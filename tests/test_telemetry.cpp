#include "sdr/telemetry/StatePacket.hpp"
#include "sdr/telemetry/TelemetryServer.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace sdr;

static StatePacket sample() {
    StatePacket s;
    s.node_id = 0xDEADBEEF; s.uptime_s = 12345; s.mode = 1;
    s.freq_tx_hz = 434000000u; s.freq_rx_hz = 439000000u; s.bw_hz = 1000000u;
    s.sps = 4; s.modulation = 2; s.tx_atten_cdb = 1025;      // 10.25 dB
    s.frames_tx = 1ull << 40; s.frames_rx_good = 999; s.frames_rx_bad = 7;
    s.dropped = 3; s.bytes_tx = 1234567890ull; s.bytes_rx = 42; s.fec_corrected = 5;
    s.rssi_cdbm = -6350; s.snr_cdb = 1875;                    // -63.50 dBm, 18.75 dB
    s.tx_kbps = 700; s.rx_kbps = 690; s.tx_duty_pct = 65; s.temp_cc = 4225;
    s.fpga_magic = 0x5344524C; s.fpga_version = 0x00010300;
    s.fpga_abi = 3; s.regmap_ver = 3;
    return s;
}

static void test_roundtrip() {
    auto a = sample();
    auto w = a.encode();
    auto b = StatePacket::decode(w.data(), w.size());
    assert(b.has_value());
    assert(b->node_id == a.node_id && b->uptime_s == a.uptime_s && b->mode == a.mode);
    assert(b->freq_tx_hz == a.freq_tx_hz && b->freq_rx_hz == a.freq_rx_hz);
    assert(b->bw_hz == a.bw_hz && b->sps == a.sps && b->modulation == a.modulation);
    assert(b->tx_atten_cdb == a.tx_atten_cdb);
    assert(b->frames_tx == a.frames_tx && b->bytes_tx == a.bytes_tx);
    // Signed fields must survive: a negative RSSI read back positive would look
    // like a very strong signal rather than a decode fault.
    assert(b->rssi_cdbm == -6350 && b->snr_cdb == 1875);
    assert(b->fpga_magic == 0x5344524Cu && b->fpga_abi == 3);
    std::cout << "  [telemetry] round trip: PASS\n";
}

static void test_rejects_corruption() {
    auto w = sample().encode();
    for (size_t i = 0; i < w.size(); ++i) {
        auto bad = w; bad[i] ^= 0x01;
        // Every single-bit flip anywhere must be rejected. A corrupt report
        // that decodes is worse than none: it is indistinguishable from truth.
        assert(!StatePacket::decode(bad.data(), bad.size()).has_value());
    }
    std::cout << "  [telemetry] every single-bit flip rejected: PASS\n";
}

static void test_rejects_truncation_and_junk() {
    auto w = sample().encode();
    for (size_t n = 0; n < w.size(); ++n)
        assert(!StatePacket::decode(w.data(), n).has_value());
    assert(!StatePacket::decode(nullptr, 0).has_value());
    std::vector<uint8_t> junk(64, 0xA5);
    assert(!StatePacket::decode(junk.data(), junk.size()).has_value());
    std::cout << "  [telemetry] truncation and junk rejected: PASS\n";
}

static void test_forward_compatible() {
    // A NEWER sender appends fields. We must read what we know and ignore the
    // rest, rather than reject -- that promise is the reason body_len exists.
    auto a = sample();
    auto w = a.encode();
    size_t blen = size_t(w[6]) | (size_t(w[7]) << 8);
    std::vector<uint8_t> ext(w.begin(), w.begin() + StatePacket::HEADER_BYTES + blen);
    const std::vector<uint8_t> extra = {0xAA, 0xBB, 0xCC, 0xDD};
    ext.insert(ext.end(), extra.begin(), extra.end());
    size_t nlen = blen + extra.size();
    ext[6] = uint8_t(nlen & 0xFF); ext[7] = uint8_t((nlen >> 8) & 0xFF);
    // Recompute the CRC the way a real newer sender would.
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t byte : ext) { c ^= byte; for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u)); }
    c = ~c;
    for (int i = 0; i < 4; ++i) ext.push_back((c >> (8*i)) & 0xFF);

    auto b = StatePacket::decode(ext.data(), ext.size());
    assert(b.has_value());
    assert(b->node_id == a.node_id && b->fpga_abi == a.fpga_abi);
    std::cout << "  [telemetry] newer sender with extra fields still parses: PASS\n";
}

static void test_short_body_defaults() {
    // An OLDER sender omits the tail. Those fields must come back as defaults,
    // not as garbage read past the end of the body.
    auto a = sample();
    auto w = a.encode();
    size_t blen = size_t(w[6]) | (size_t(w[7]) << 8);
    size_t cut  = blen - 10;                     // drop the FPGA identity tail
    std::vector<uint8_t> shortp(w.begin(), w.begin() + StatePacket::HEADER_BYTES + cut);
    shortp[6] = uint8_t(cut & 0xFF); shortp[7] = uint8_t((cut >> 8) & 0xFF);
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t byte : shortp) { c ^= byte; for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u)); }
    c = ~c;
    for (int i = 0; i < 4; ++i) shortp.push_back((c >> (8*i)) & 0xFF);

    auto b = StatePacket::decode(shortp.data(), shortp.size());
    assert(b.has_value());
    assert(b->node_id == a.node_id);
    assert(b->fpga_magic == 0 && b->fpga_abi == 0);
    std::cout << "  [telemetry] older sender's missing tail reads as defaults: PASS\n";
}

static void test_json_has_units() {
    auto j = sample().toJSON();
    assert(j.find("\"rssi_dbm\": -63.50") != std::string::npos);
    assert(j.find("\"freq_tx_mhz\": 434.00") != std::string::npos);
    assert(j.find("\"modulation\": \"QPSK\"") != std::string::npos);
    std::cout << "  [telemetry] JSON renders scaled units: PASS\n";
}

// The server and a client, over a real loopback socket. Exercises the path
// sdrctl actually uses, rather than asserting that encode and decode agree with
// each other -- which they would even if the transport were broken.
static void test_server_round_trip() {
    StatePacket ref = sample();
    TelemetryServer srv([&](StatePacket& p) { p = ref; }, "127.0.0.1", 55140);
    if (!srv.start()) { std::cout << "  [telemetry] server bind failed: SKIP\n"; return; }

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(55140);
    assert(::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) == 1);
    timeval tv{}; tv.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    assert(::sendto(fd, "STAT", 4, 0, (sockaddr*)&a, sizeof a) == 4);
    uint8_t buf[2048];
    ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);
    assert(n > 0);
    auto got = StatePacket::decode(buf, (size_t)n);
    assert(got.has_value());
    assert(got->node_id == ref.node_id && got->rssi_cdbm == ref.rssi_cdbm);

    // An unrecognised request must draw no reply at all: a server that answers
    // arbitrary datagrams can be pointed at a third party as an amplifier.
    assert(::sendto(fd, "XXXX", 4, 0, (sockaddr*)&a, sizeof a) == 4);
    tv.tv_sec = 0; tv.tv_usec = 300000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    assert(::recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr) < 0);

    ::close(fd);
    srv.stop();
    assert(!srv.running());
    std::cout << "  [telemetry] server answers STAT, ignores junk, stops cleanly: PASS\n";
}

void run_telemetry() {
    std::cout << "[telemetry]\n";
    test_roundtrip();
    test_rejects_corruption();
    test_rejects_truncation_and_junk();
    test_forward_compatible();
    test_short_body_defaults();
    test_json_has_units();
    test_server_round_trip();
}
