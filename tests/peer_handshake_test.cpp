// Acceptance tests for the peer identity handshake.
//
// The requirement: swapping in a misconfigured or duplicate-ID unit must
// produce a clear PEER_INCOMPATIBLE state, not silent packet loss.
#include "sdr/bridge/PeerHandshake.hpp"
#include <cstdio>
#include <string>

using namespace sdr;
static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %-62s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}
static bool mentions(const PeerCheck& c, const std::string& frag) {
    for (auto& r : c.reasons) if (r.find(frag) != std::string::npos) return true;
    return false;
}
// A correctly configured pair: frequencies crossed, distinct ids.
static void pair(PeerIdentity& a, PeerIdentity& b) {
    a = PeerIdentity{}; b = PeerIdentity{};
    a.node_id=1; b.node_id=2;
    a.tx_freq=434000000; a.rx_freq=444000000;
    b.tx_freq=444000000; b.rx_freq=434000000;
    a.sample_rate=b.sample_rate=15360000;
    a.diff_mode=b.diff_mode=1;
    a.pkt_bytes=b.pkt_bytes=32768;
    a.fpga_abi=b.fpga_abi=3; a.fpga_map=b.fpga_map=3;
    a.sw_version=b.sw_version=0xABCD;
}

int main() {
    PeerIdentity a, b;

    std::printf("wire format\n");
    pair(a,b);
    { auto w = encodeHello(a); PeerIdentity r;
      check(decodeHello(w.data(), w.size(), r), "a HELLO round-trips");
      check(r.node_id==a.node_id && r.tx_freq==a.tx_freq && r.diff_mode==a.diff_mode,
            "fields survive the round trip"); }
    { auto w = encodeHello(a); w[0] ^= 0xFF; PeerIdentity r;
      check(!decodeHello(w.data(), w.size(), r), "a corrupted magic is rejected"); }
    { auto w = encodeHello(a); w[4] = 99; PeerIdentity r;
      check(!decodeHello(w.data(), w.size(), r), "an unknown version is NOT assumed compatible"); }
    { PeerIdentity r; uint8_t sh[8] = {0};
      check(!decodeHello(sh, sizeof sh, r), "a short payload is rejected, not read past"); }

    std::printf("\nthe failure this was written for\n");
    pair(a,b); b.node_id = a.node_id;            // both left on the default
    { auto c = checkPeer(a,b);
      check(c.verdict == PeerVerdict::INCOMPATIBLE, "duplicate node_id -> PEER_INCOMPATIBLE");
      check(mentions(c,"duplicate node_id"), "the reason names the duplicate id"); }

    std::printf("\nfrequency misconfiguration\n");
    pair(a,b); b.tx_freq = b.rx_freq = 434000000; // peer not crossed: jams itself
    { auto c = checkPeer(a,b);
      check(c.verdict == PeerVerdict::INCOMPATIBLE, "peer with tx==rx -> INCOMPATIBLE");
      check(mentions(c,"our rx"), "the reason names the frequency that does not line up"); }
    pair(a,b); b.tx_freq = 434000000; b.rx_freq = 444000000;  // same plan, not crossed
    { auto c = checkPeer(a,b);
      check(c.verdict == PeerVerdict::INCOMPATIBLE, "uncrossed pair -> INCOMPATIBLE"); }

    std::printf("\nother silent-total-loss combinations\n");
    pair(a,b); b.diff_mode = 0;
    check(checkPeer(a,b).verdict == PeerVerdict::INCOMPATIBLE, "diff_mode mismatch -> INCOMPATIBLE");
    pair(a,b); b.sample_rate = 7680000;
    check(checkPeer(a,b).verdict == PeerVerdict::INCOMPATIBLE, "sample-rate mismatch -> INCOMPATIBLE");
    pair(a,b); b.pkt_bytes = 8192;
    check(checkPeer(a,b).verdict == PeerVerdict::INCOMPATIBLE, "PKT_BYTES mismatch -> INCOMPATIBLE");
    pair(a,b); b.fpga_abi = 4;
    check(checkPeer(a,b).verdict == PeerVerdict::INCOMPATIBLE, "FPGA ABI mismatch -> INCOMPATIBLE");
    pair(a,b); b.fpga_map = 4;
    check(checkPeer(a,b).verdict == PeerVerdict::INCOMPATIBLE, "register-map mismatch -> INCOMPATIBLE");

    std::printf("\na correctly configured pair, and advisory differences\n");
    pair(a,b);
    { auto c = checkPeer(a,b);
      check(c.verdict == PeerVerdict::COMPATIBLE, "the crossed, distinct-id pair is COMPATIBLE");
      check(c.reasons.empty(), "no refusal reasons"); }
    pair(a,b); b.sw_version = 0x1234;
    { auto c = checkPeer(a,b);
      check(c.verdict == PeerVerdict::COMPATIBLE, "differing software does NOT block forwarding");
      check(c.warnings.size() == 1, "but it is reported as a warning"); }

    std::printf("\nmultiple faults are all reported, not just the first\n");
    pair(a,b); b.node_id = a.node_id; b.diff_mode = 0; b.sample_rate = 7680000;
    { auto c = checkPeer(a,b);
      check(c.reasons.size() >= 3, "three faults give at least three reasons (got " + std::to_string(c.reasons.size()) + ")"); }

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
