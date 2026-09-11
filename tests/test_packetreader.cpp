#include "sdr/framing/PacketReader.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/OffsetDeframer.hpp"
#include "sdr/framing/Frame.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace sdr;

namespace {

std::vector<uint8_t> makePayload(size_t len, uint32_t seed) {
    std::vector<uint8_t> p(len);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : p) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = static_cast<uint8_t>(x >> 24); }
    return p;
}

// A writer that fragments deliberately and pathologically: 1-byte dribbles,
// odd sizes, and chunks that straddle packet boundaries. A pipe in the field
// is only ever *more* convenient than this.
void hostileWriter(int fd, std::vector<uint8_t> data) {
    uint32_t x = 0x1234567u;
    size_t off = 0;
    while (off < data.size()) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        size_t chunk = 1 + (x % 997);
        if (chunk > data.size() - off) chunk = data.size() - off;
        size_t done = 0;
        while (done < chunk) {
            ssize_t w = ::write(fd, data.data() + off + done, chunk - done);
            if (w <= 0) { ::close(fd); return; }
            done += static_cast<size_t>(w);
        }
        off += chunk;
    }
    ::close(fd);
}

// THE ACCEPTANCE CRITERION: pipe fragmentation must never cause a valid frame
// to be discarded. Build a stream of whole packets, deliver it through a pipe
// that shreds it into arbitrary pieces, and require every frame back.
void test_fragmentation_loses_nothing() {
    const size_t PKT = 32768;
    const int    NPKT = 6;
    const size_t paylen = 1200;

    Framer framer;
    std::vector<uint8_t> stream;
    std::vector<std::vector<uint8_t>> sent;
    uint32_t seq = 0;
    // Fill whole packets with whole frames, so nothing straddles a boundary
    // and every frame is recoverable in principle.
    for (int p = 0; p < NPKT; ++p) {
        std::vector<uint8_t> pkt;
        while (pkt.size() + 1400 < PKT) {
            auto pay = makePayload(paylen, seq);
            auto w = framer.encode(pay, 0, ModCode::QPSK, BwCode::BW_5, 7, seq, nullptr, nullptr);
            pkt.insert(pkt.end(), w.begin(), w.end());
            sent.push_back(std::move(pay));
            ++seq;
        }
        pkt.resize(PKT, 0);                       // pad to a full packet
        stream.insert(stream.end(), pkt.begin(), pkt.end());
    }

    int fds[2];
    assert(::pipe(fds) == 0);
    std::thread w(hostileWriter, fds[1], stream);

    OffsetDeframer d;
    std::vector<uint8_t> buf(PKT);
    size_t packets = 0, frames = 0;
    while (true) {
        ssize_t n = readExact(fds[0], buf.data(), buf.size());
        if (n <= 0) break;
        // Every read must be a WHOLE packet; a short one only at end of stream.
        assert(n == static_cast<ssize_t>(PKT));
        ++packets;
        frames += d.pushPacket(buf.data(), static_cast<size_t>(n)).size();
    }
    w.join();
    ::close(fds[0]);

    std::cout << "    " << packets << "/" << NPKT << " packets reassembled, "
              << frames << "/" << sent.size() << " frames recovered\n";
    assert(packets == static_cast<size_t>(NPKT));
    // Not "most": every frame. A fragment decoded as a packet would lose the
    // frames at both of its ends, which is precisely what this guards against.
    assert(frames == sent.size());
    assert(d.crcErrors() == 0);
}

// A stream that ends mid-packet must report the partial count, not silently
// present it as a whole packet -- otherwise a truncated transfer is decoded as
// if the missing bytes were zeros.
void test_truncated_stream_reports_short() {
    int fds[2];
    assert(::pipe(fds) == 0);
    std::vector<uint8_t> half(5000, 0xA5);
    std::thread w(hostileWriter, fds[1], half);

    std::vector<uint8_t> buf(32768);
    ssize_t n = readExact(fds[0], buf.data(), buf.size());
    w.join();
    ::close(fds[0]);
    std::cout << "    truncated stream returned " << n << " of 32768 (expected 5000)\n";
    assert(n == 5000);
}

// A single byte at a time is the worst case a pipe can produce.
void test_byte_at_a_time() {
    int fds[2];
    assert(::pipe(fds) == 0);
    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 31 + 7);

    std::thread w([fd = fds[1], data]() {
        for (size_t i = 0; i < data.size(); ++i)
            if (::write(fd, data.data() + i, 1) != 1) break;
        ::close(fd);
    });

    std::vector<uint8_t> buf(4096);
    ssize_t n = readExact(fds[0], buf.data(), buf.size());
    w.join();
    ::close(fds[0]);
    assert(n == 4096);
    assert(std::memcmp(buf.data(), data.data(), 4096) == 0);
    std::cout << "    1-byte-at-a-time delivery reassembled exactly\n";
}

} // namespace

void run_packetreader() {
    std::cout << "  [packetreader] DMA packet reassembly across a fragmenting pipe\n";
    test_byte_at_a_time();
    test_truncated_stream_reports_short();
    test_fragmentation_loses_nothing();
}
