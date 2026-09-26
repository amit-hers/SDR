// Acceptance tests for the active health-probe wire format.
//
// The requirement: a request round-trips through encode/decode with its
// sequence number and send-time intact, a reply is distinguishable from a
// request by magic alone, and a corrupted or short frame is rejected
// outright rather than partially parsed.
#include "sdr/bridge/HealthProbe.hpp"
#include <cstdio>
#include <string>

using namespace sdr;
static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %-62s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    std::printf("wire format\n");
    {
        ProbeMessage m; m.seq = 42; m.send_time_us = 1234567890123ULL;
        auto w = encodeProbe(PROBE_REQUEST_MAGIC, m);
        check(w.size() == PROBE_SIZE, "a request encodes to the fixed PROBE_SIZE");
        ProbeMessage r;
        check(decodeProbe(w.data(), w.size(), r) == ProbeKind::REQUEST, "it decodes back as a REQUEST");
        check(r.seq == m.seq, "sequence number survives the round trip");
        check(r.send_time_us == m.send_time_us, "send time survives the round trip, unmodified");
    }
    {
        ProbeMessage m; m.seq = 7; m.send_time_us = 999;
        auto w = encodeProbe(PROBE_REPLY_MAGIC, m);
        ProbeMessage r;
        check(decodeProbe(w.data(), w.size(), r) == ProbeKind::REPLY, "a reply decodes as REPLY, not REQUEST");
        check(r.seq == 7 && r.send_time_us == 999, "reply fields survive the round trip");
    }
    {
        ProbeMessage m; auto w = encodeProbe(PROBE_REQUEST_MAGIC, m);
        w[0] ^= 0xFF;
        ProbeMessage r;
        check(decodeProbe(w.data(), w.size(), r) == ProbeKind::NONE, "a corrupted magic is rejected");
    }
    {
        ProbeMessage m; auto w = encodeProbe(PROBE_REQUEST_MAGIC, m);
        w[4] = 99;   // version byte
        ProbeMessage r;
        check(decodeProbe(w.data(), w.size(), r) == ProbeKind::NONE, "an unknown version is NOT assumed compatible");
    }
    {
        uint8_t shortbuf[8] = {0};
        ProbeMessage r;
        check(decodeProbe(shortbuf, sizeof shortbuf, r) == ProbeKind::NONE, "a short payload is rejected, not read past");
    }
    {
        // HELLO_SIZE (60 bytes) and PROBE_SIZE (17 bytes) must stay distinct
        // enough that a HELLO can never accidentally look like a probe frame
        // by size alone -- decodeHello/decodeProbe are tried independently on
        // every FL_CTRL frame in sdr_bridge.cpp, so this needs to hold.
        check(PROBE_SIZE < 60, "PROBE_SIZE stays well under HELLO_SIZE (60 bytes)");
    }
    {
        // A stray non-probe payload of exactly PROBE_SIZE bytes (e.g. random
        // noise decoded as a frame) must not be misread as a valid probe --
        // the magic check is what prevents that, not the length alone.
        uint8_t junk[PROBE_SIZE];
        for (std::size_t i = 0; i < sizeof junk; ++i) junk[i] = static_cast<uint8_t>(i * 37 + 11);
        ProbeMessage r;
        check(decodeProbe(junk, sizeof junk, r) == ProbeKind::NONE, "arbitrary PROBE_SIZE-length bytes are not read as a probe");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
