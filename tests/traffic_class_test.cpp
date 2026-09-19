// Acceptance tests for traffic classification and bounded priority scheduling.
#include "sdr/bridge/TrafficClass.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sdr;
static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %-60s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}
static std::vector<uint8_t> frame(uint16_t et, std::size_t n, uint8_t pcp = 0xFF) {
    std::vector<uint8_t> f(n, 0xAA);
    f[12] = uint8_t(et >> 8); f[13] = uint8_t(et & 0xFF);
    if (et == 0x8100 && n >= 16 && pcp != 0xFF) f[14] = uint8_t(pcp << 5);
    return f;
}

int main() {
    std::printf("classification\n");
    { auto f = frame(0x0800, 64);   check(classify(f.data(), f.size()) == Class::CONTROL, "small IPv4 (64 B) -> CONTROL"); }
    { auto f = frame(0x0800, 1400); check(classify(f.data(), f.size()) == Class::BULK,    "large IPv4 (1400 B) -> BULK"); }
    { auto f = frame(0x0806, 1400); check(classify(f.data(), f.size()) == Class::CONTROL, "ARP stays CONTROL even when large"); }
    { auto f = frame(0x8100, 1400, 6); check(classify(f.data(), f.size()) == Class::CONTROL, "802.1Q PCP 6 overrides size -> CONTROL"); }
    { auto f = frame(0x8100, 64, 1);   check(classify(f.data(), f.size()) == Class::BULK,    "802.1Q PCP 1 overrides size -> BULK"); }
    { std::vector<uint8_t> r(8, 0);    check(classify(r.data(), r.size()) == Class::CONTROL, "runt shorter than a header -> CONTROL"); }
    { auto f = frame(0x86DD, 64); f[20] = 58; check(classify(f.data(), f.size()) == Class::CONTROL, "IPv6 ICMPv6/ND -> CONTROL"); }

    std::printf("\npriority: control overtakes queued bulk\n");
    {
        TrafficQueues q(256, 256, 8);
        for (int i = 0; i < 10; ++i) { auto b = frame(0x0800, 1400); q.push(b.data(), b.size()); }
        auto c = frame(0x0800, 64); q.push(c.data(), c.size());      // queued LAST
        std::vector<uint8_t> out;
        q.pop(out);
        check(out.size() == 64, "a control frame queued behind 10 bulk frames leaves first");
    }

    std::printf("\nstarvation protection\n");
    {
        TrafficQueues q(1024, 1024, 8);
        for (int i = 0; i < 50; ++i) { auto b = frame(0x0800, 1400); q.push(b.data(), b.size()); }
        for (int i = 0; i < 200; ++i) { auto c = frame(0x0800, 64); q.push(c.data(), c.size()); }
        int bulk_sent = 0; std::vector<uint8_t> out;
        for (int i = 0; i < 100 && q.pop(out); ++i) if (out.size() == 1400) ++bulk_sent;
        check(bulk_sent > 0, "bulk still progresses under a continuous control stream");
        check(bulk_sent >= 10 && bulk_sent <= 15, "bulk share is bounded near 1-in-8 (got " + std::to_string(bulk_sent) + "/100)");
    }

    std::printf("\nbounded queues\n");
    {
        TrafficQueues q(4, 4, 8);
        int accepted = 0;
        for (int i = 0; i < 100; ++i) { auto b = frame(0x0800, 1400); if (q.push(b.data(), b.size())) ++accepted; }
        check(accepted == 4, "bulk queue accepts only its limit (memory cannot grow without bound)");
        check(q.bulkDropped() == 96, "drops are counted, not silent");
        check(q.controlDepth() == 0 && q.bulkDepth() == 4, "depths are observable");
    }

    std::printf("\ncontrol flood does not evict bulk\n");
    {
        TrafficQueues q(4, 4, 8);
        auto b = frame(0x0800, 1400); q.push(b.data(), b.size());
        for (int i = 0; i < 50; ++i) { auto c = frame(0x0800, 64); q.push(c.data(), c.size()); }
        check(q.bulkDepth() == 1, "a control flood fills only the control queue");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
