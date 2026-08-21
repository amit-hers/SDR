#pragma once
#include "sdr/framing/Frame.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

namespace sdr {

// Packing and unpacking of several TAP packets inside one RF payload.
//
// Kept out of the daemon so it can be tested directly: the first version of
// this lived inline in the RX loop, and a fault there is invisible to every
// existing test while looking, from the statistics, like a radio problem --
// frames decode, CRC passes, and the packets simply never arrive.
//
// Layout, repeated until the payload is consumed:
//
//     [ uint16 big-endian length ][ length bytes of packet ]
namespace aggregate {

// Bytes this packet will occupy once appended.
inline size_t costOf(size_t pkt_len) { return AGGR_LEN_PREFIX + pkt_len; }

inline void append(std::vector<uint8_t>& buf, const uint8_t* pkt, size_t len) {
    buf.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(len & 0xFF));
    buf.insert(buf.end(), pkt, pkt + len);
}

// Walks the records, calling emit(ptr, len) for each. Returns the number of
// records emitted. Stops at the first malformed length rather than reading
// past the end -- the CRC has already passed by this point, so that only
// guards against a corrupt-but-CRC-valid frame.
template <typename Emit>
size_t split(const uint8_t* data, size_t size, Emit emit) {
    size_t off = 0, n = 0;
    while (off + AGGR_LEN_PREFIX <= size) {
        size_t len = (static_cast<size_t>(data[off]) << 8) | data[off + 1];
        off += AGGR_LEN_PREFIX;
        if (len == 0 || off + len > size) break;
        emit(data + off, len);
        off += len;
        ++n;
    }
    return n;
}

} // namespace aggregate
} // namespace sdr
