#pragma once
// Every acceptance test must prove its preconditions before evaluating its
// result.
//
// WHY THIS IS A HEADER AND NOT A CONVENTION. Three separate test suites in this
// project reported green while exercising nothing:
//
//   * peer-handshake fixtures left both MACs zeroed, so every "pair" compared a
//     unit against itself -- 13 assertions passed for the wrong reason
//   * the config-schema harness never regenerated its file, so each negative
//     case re-validated the previous case's already-invalid config -- 15 false
//     passes
//   * RF measurements ran past the end of the transmit feed, so the tail
//     recorded silence and "the demodulator saturates" was a fixture artifact
//
// In each case the assertion under test was correct and the SETUP was not. A
// precondition that fails must therefore abort the test rather than let it
// report anything, because a green result from an unexercised fixture is worse
// than a red one: it closes the question.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sdr { namespace test {

inline void require(bool ok, const std::string& what) {
    std::printf("  [precondition] %-46s %s\n", what.c_str(), ok ? "ok" : "NOT MET");
    if (!ok) {
        std::printf("\nABORTED: a precondition failed, so no result below would mean\n"
                    "anything. Fix the fixture, not the assertion.\n");
        std::exit(2);
    }
}

// Two units are only a pair if their authoritative identities differ. This is
// the specific check whose absence produced the 13 false passes.
inline void requireDistinctUnits(const uint8_t* mac_a, const uint8_t* mac_b) {
    require(std::memcmp(mac_a, mac_b, 6) != 0,
            "fixture MACs differ (these are two units, not one)");
}

}} // namespace sdr::test
