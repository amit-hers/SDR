#pragma once
#include "sdr/framing/Frame.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace sdr {

// The host/radio boundary for a DTC-style split.
//
// Today the daemon owns the entire physical layer: it pulls raw IQ from the
// radio over Ethernet, runs burst detection, timing recovery, carrier
// recovery, demodulation and deframing on the host CPU, and pushes modulated
// IQ back. That makes Ethernet traffic a function of SAMPLE RATE -- measured
// at 2 MHz QPSK, 415 Mbps of IQ crossed the link to deliver 1.19 Mbps of
// payload, a ratio of 347x -- and it is why 4 MHz does not fit on a gigabit
// link shared by two radios.
//
// A DTC radio moves the physical layer into the radio, so what crosses
// Ethernet is FRAMES. Traffic then scales with payload and is independent of
// sample rate: 2, 4, 10 or 20 MHz all cost the same ~1-26 Mbps depending only
// on duty cycle and modulation order.
//
// This interface is what the daemon talks to in either case. It deliberately
// exposes no notion of samples: anything sample-rate-dependent belongs on the
// radio side of the boundary. The two implementations are:
//
//   SoftwarePhy  owns a PlutoSDR, streams IQ, and runs the existing DSP chain
//                on the host. Behaviourally identical to today.
//   RadioPhy     speaks DtcProtocol to a radio that runs the PHY in fabric.
//
// The daemon keeps what the host is genuinely better at and what does not
// scale with sample rate: TAP/IP handling, aggregation, ARQ, routing, MANET
// logic, link adaptation and control.
class IFramePhy {
public:
    virtual ~IFramePhy() = default;

    // ── Identity ─────────────────────────────────────────────────────────
    enum class Kind {
        SOFTWARE,   // host runs the PHY; IQ crosses the link
        RADIO,      // radio runs the PHY; frames cross the link
    };
    virtual Kind kind() const = 0;

    // ── Configuration ────────────────────────────────────────────────────
    // Applied atomically where the transport allows it. Returns false and
    // leaves the previous configuration in force if the radio rejects it, so
    // a failed link-adaptation step cannot leave the PHY in a half-applied
    // state.
    struct Config {
        uint64_t tx_freq_hz {0};
        uint64_t rx_freq_hz {0};
        uint32_t symbol_rate {0};       // symbols/second on the air
        ModCode  tx_mod {ModCode::QPSK};// acquisition stays BPSK regardless
        float    tx_atten_db {25.0f};
        float    tx_duty_max {0.65f};   // enforced radio-side in DTC mode
        bool     fec {false};
        bool     encrypt {false};
    };
    virtual bool configure(const Config& cfg) = 0;

    // ── Data plane ───────────────────────────────────────────────────────
    // Hand one payload to the PHY for transmission. The PHY owns framing,
    // modulation, shaping and duty pacing; the caller owns aggregation.
    //
    // Returns false when the PHY cannot accept it, which is backpressure and
    // not an error -- the caller must not retry blindly, it must let its own
    // ingress queue fill and drop there, where the loss is visible. Silent
    // acceptance followed by an internal discard is exactly the bug this
    // codebase already had once.
    virtual bool sendFrame(const uint8_t* payload, size_t len,
                           uint8_t flags, uint32_t seq) = 0;

    // Send anything the PHY is holding back, now.
    //
    // A PHY batches frames so each transmission is worth its fixed overhead,
    // and it cannot tell "more frames are coming in a moment" from "the
    // offered traffic has stopped" -- only the caller knows that. So the
    // caller says. Without this the PHY has to guess from its own queue
    // depth, which under load looks idle every few milliseconds and produces
    // half-filled bursts: measured 3.66 frames per burst instead of 6.4, and
    // the peer then sees twice as many half-length windows.
    virtual void flushPending() = 0;

    // Decoded frames, delivered as they arrive. Called on the PHY's own
    // thread; the callee must not block it.
    using FrameHandler = std::function<void(const DecodedFrame&)>;
    virtual void onFrame(FrameHandler h) = 0;

    // ── Accounting ───────────────────────────────────────────────────────
    // Deliberately mirrors the counters the software path already reports, so
    // a DTC radio can be held to the same standard rather than being trusted.
    // Every frame offered must land in exactly one of decoded / crc_failed /
    // dropped; that invariant is the acceptance test for a radio-side PHY.
    struct Stats {
        uint64_t frames_offered {0};    // handed to sendFrame and accepted
        uint64_t frames_on_air {0};     // actually transmitted
        uint64_t frames_dropped {0};    // accepted then discarded -- must be 0
        uint64_t frames_decoded {0};
        uint64_t frames_crc_failed {0};
        uint64_t bursts_detected {0};
        uint64_t bytes_delivered {0};
        // Link traffic, so the DTC scaling claim is measured and not assumed.
        uint64_t host_link_tx_bytes {0};
        uint64_t host_link_rx_bytes {0};
        // Cumulative seconds of signal actually radiated. The daemon takes
        // the delta per tick to get live duty; only the PHY can know it,
        // since only the PHY knows how many samples reached the air.
        double   air_seconds {0.0};
        float    tx_duty_pct {0.f};
        float    snr_db {0.f};
        float    rssi_dbm {0.f};
        float    temp_c {0.f};
    };
    virtual Stats stats() const = 0;

    // ── Lifecycle ────────────────────────────────────────────────────────
    virtual bool start() = 0;
    virtual void stop() = 0;
};

} // namespace sdr
