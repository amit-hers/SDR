// Acceptance tests for L2 loop suppression.
//
// The requirement is that two appliances on one segment must never replicate
// uncontrollably, for EVERY Ethernet frame class -- broadcast, multicast, ARP,
// IPv6 ND, unknown unicast -- and that a station which genuinely moves between
// sides is relearned rather than blocked for ever.
#include "sdr/bridge/LoopGuard.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sdr;
using clk = LoopGuard::clock;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// dst, src, ethertype, payload
static std::vector<uint8_t> frame(const uint8_t* dst, const uint8_t* src,
                                  uint16_t et, std::size_t pay = 46) {
    std::vector<uint8_t> f(14 + pay, 0);
    std::memcpy(f.data(), dst, 6);
    std::memcpy(f.data() + 6, src, 6);
    f[12] = uint8_t(et >> 8); f[13] = uint8_t(et & 0xFF);
    return f;
}

int main() {
    const uint8_t H[6]     = {0x02,0x11,0x22,0x33,0x44,0x55};   // a station
    const uint8_t OTHER[6] = {0x02,0xAA,0xBB,0xCC,0xDD,0xEE};
    const uint8_t BCAST[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    const uint8_t MCAST[6] = {0x01,0x00,0x5e,0x01,0x02,0x03};   // IPv4 multicast
    const uint8_t ND6[6]   = {0x33,0x33,0xff,0x00,0x00,0x01};   // IPv6 ND solicited-node
    auto t0 = clk::now();

    std::printf("frame classes -- a radio-side source must never go back to radio\n");
    struct { const char* name; const uint8_t* dst; uint16_t et; } cases[] = {
        {"broadcast (dst ff:ff:ff:ff:ff:ff)", BCAST, 0x0800},
        {"IPv4 multicast (dst 01:00:5e:..)",  MCAST, 0x0800},
        {"ARP (ethertype 0x0806, broadcast)", BCAST, 0x0806},
        {"IPv6 ND (dst 33:33:ff:..)",         ND6,   0x86DD},
        {"unknown unicast (dst never seen)",  OTHER, 0x88B5},
    };
    for (auto& c : cases) {
        LoopGuard g;
        auto f = frame(c.dst, H, c.et);
        check(g.shouldForwardToRadio(f.data(), f.size(), t0),
              (std::string("before learning, forwarded: ") + c.name).c_str());
        g.learnFromRadio(f.data(), f.size(), t0);
        check(!g.shouldForwardToRadio(f.data(), f.size(), t0),
              (std::string("after radio sighting, SUPPRESSED: ") + c.name).c_str());
    }

    std::printf("\nthe loop itself\n");
    {
        // Two units, each with its own guard, on one segment.
        LoopGuard A, B;
        auto f = frame(BCAST, H, 0x0800);
        // step 2: neither knows H from radio yet, so both forward
        bool a1 = A.shouldForwardToRadio(f.data(), f.size(), t0);
        bool b1 = B.shouldForwardToRadio(f.data(), f.size(), t0);
        check(a1 && b1, "step 2: both units forward the original to RF");
        // step 3: each receives the other's transmission and injects
        A.learnFromRadio(f.data(), f.size(), t0);
        B.learnFromRadio(f.data(), f.size(), t0);
        // step 4: each sees the other's injection returning over Ethernet
        bool a2 = A.shouldForwardToRadio(f.data(), f.size(), t0);
        bool b2 = B.shouldForwardToRadio(f.data(), f.size(), t0);
        check(!a2 && !b2, "step 4: both DROP the echo -- loop terminates");
    }

    std::printf("\nMAC movement and ageing\n");
    {
        LoopGuard g(std::chrono::seconds(2));
        auto f = frame(OTHER, H, 0x0800);
        g.learnFromRadio(f.data(), f.size(), t0);
        check(!g.shouldForwardToRadio(f.data(), f.size(), t0), "suppressed while the sighting is fresh");
        check(g.shouldForwardToRadio(f.data(), f.size(), t0 + std::chrono::seconds(3)),
              "relearned after TTL: a station that moved is not blocked for ever");
    }

    std::printf("\nmalformed and hostile input\n");
    {
        LoopGuard g;
        // A source address with the multicast bit set is invalid per 802.3.
        // Learning it would let a crafted frame suppress a real station.
        auto bad = frame(BCAST, BCAST, 0x0800);
        g.learnFromRadio(bad.data(), bad.size(), t0);
        check(g.size() == 0, "multicast source never learned (cannot poison the table)");
        std::vector<uint8_t> runt(10, 0);
        check(g.shouldForwardToRadio(runt.data(), runt.size(), t0), "frame shorter than a header is forwarded, not crashed");

        // Table growth is bounded: a flood of distinct sources must not grow
        // without limit on a board with no swap.
        LoopGuard cap(LoopGuard::DEFAULT_TTL, 64);
        for (int i = 0; i < 500; ++i) {
            uint8_t s[6] = {0x02, uint8_t(i>>8), uint8_t(i), 0, 0, 1};
            auto ff = frame(OTHER, s, 0x0800);
            cap.learnFromRadio(ff.data(), ff.size(), t0 + std::chrono::milliseconds(i));
        }
        check(cap.size() <= 64, "table bounded under a 500-source flood");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
