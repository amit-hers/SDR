// A frame that straddles a DMA packet boundary must still be delivered.
//
// The demodulated byte stream is cut every PKT_BYTES with no regard for frame
// boundaries, and decoding restarts per packet, so a frame spanning the cut
// used to vanish with NO counter moving -- not a CRC error, not a drop, not an
// oversize. With 1514-byte frames in 8192-byte packets that is up to 19% of
// frames, and because the packet phase is fixed for a demodulator session it
// presents as "large frames never work" or "large frames always work"
// depending on the reboot. Measured on hardware: full-size pings at 0% loss in
// one session and 100% in the next, with identical configuration.
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/OffsetDeframer.hpp"
#include "sdr/framing/Frame.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace sdr;

void run_offset_boundary() {
    std::cout << "[offset_boundary]\n";

    Framer f;
    // A full-size payload: the case that actually broke on hardware.
    std::vector<uint8_t> payload(MAX_PAYLOAD);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + 1);
    const auto wire = f.encode(payload.data(), payload.size(), 0, ModCode::QPSK,
                               BwCode::BW_5, 0x1234, 1, nullptr, nullptr);

    // Split the frame at many points and require recovery at each one.
    int recovered = 0, attempts = 0;
    for (size_t cut = 64; cut + 64 < wire.size(); cut += 137) {
        OffsetDeframer od;
        // A straddling frame means the packet ENDS at the cut -- the DMA
        // packet was full. So p1 gets no padding: appending any would break
        // the frame rather than split it, which is a different fault.
        std::vector<uint8_t> p1(wire.begin(), wire.begin() + cut);
        std::vector<uint8_t> p2(wire.begin() + cut, wire.end());
        p2.resize(p2.size() + 200, PREAMBLE_BYTE);   // trailing block padding
        const auto a = od.pushPacket(p1.data(), p1.size());
        const auto b = od.pushPacket(p2.data(), p2.size());
        ++attempts;
        if (a.size() + b.size() == 1) {
            const auto& got = a.empty() ? b[0] : a[0];
            if (got.payload == payload && got.node_id == 0x1234u) ++recovered;
        }
    }
    std::cout << "  recovered " << recovered << " of " << attempts
              << " straddling splits\n";
    assert(recovered == attempts && "every straddling frame must be recovered");

    // A frame wholly inside one packet is delivered exactly once, even though
    // the boundary window re-reads the head of each packet.
    {
        OffsetDeframer od;
        std::vector<uint8_t> pkt(wire);
        pkt.resize(pkt.size() + 400, PREAMBLE_BYTE);
        const auto a = od.pushPacket(pkt.data(), pkt.size());
        const auto b = od.pushPacket(pkt.data(), pkt.size());   // same bytes again
        assert(a.size() == 1 && "frame inside one packet delivered once");
        assert(b.empty() && "an identical repeat is deduplicated");
        assert(od.crcErrors() == 0 && "no CRC errors on a clean stream");
        std::cout << "  single-packet frame delivered once, repeat deduplicated\n";
    }

    // A join across genuine byte loss must not fabricate a frame: the CRC is
    // the gate, and a recovery attempt that fails must stay silent.
    {
        OffsetDeframer od;
        std::vector<uint8_t> p1(wire.begin(), wire.begin() + 900);
        std::vector<uint8_t> p2(wire.begin() + 1200, wire.end());   // 300 B lost
        const auto a = od.pushPacket(p1.data(), p1.size());
        const auto b = od.pushPacket(p2.data(), p2.size());
        assert(a.empty() && b.empty() && "a frame broken by real loss is not fabricated");
        std::cout << "  frame broken by real byte loss is not fabricated\n";
    }

    std::cout << "  [offset_boundary] PASS\n";
}
