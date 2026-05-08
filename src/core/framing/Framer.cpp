#include "sdr/framing/Framer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include <cstring>
#include <stdexcept>

namespace sdr {

// CRC-32 (IEEE 802.3 polynomial)
static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

static void put_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF; p[3] =  v        & 0xFF;
}
static void put_u16_be(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}

std::vector<uint8_t> Framer::encode(const uint8_t* payload, size_t payload_len,
                                    uint8_t  flags,
                                    ModCode  mod,
                                    BwCode   bw,
                                    uint32_t node_id,
                                    uint32_t seq,
                                    const ReedSolomon* fec,
                                    const AESCipher*   aes) {
    if (payload_len > MAX_PAYLOAD)
        throw std::length_error("Framer: payload exceeds MAX_PAYLOAD");

    // 1. Optionally FEC-encode
    std::vector<uint8_t> encoded_payload(payload, payload + payload_len);
    if (fec && (flags & FL_FEC))
        encoded_payload = fec->encode(payload, payload_len);

    // 2. Optionally encrypt
    if (aes && (flags & FL_ENCRYPT))
        aes->crypt(encoded_payload.data(), encoded_payload.size(),
                   static_cast<uint64_t>(seq));

    // 3. Build frame
    size_t plen  = encoded_payload.size();
    size_t total = FRAME_OVERHEAD + plen;
    std::vector<uint8_t> frame(total);
    uint8_t* p = frame.data();

    put_u32_be(p, FRAME_SYNC);                               p += 4;
    *p++ = FRAME_VER;
    *p++ = flags;
    *p++ = static_cast<uint8_t>(mod);
    *p++ = static_cast<uint8_t>(bw);
    put_u32_be(p, node_id);                                  p += 4;
    put_u32_be(p, seq);                                      p += 4;
    put_u16_be(p, static_cast<uint16_t>(payload_len));       p += 2;
    std::memcpy(p, encoded_payload.data(), plen);            p += plen;

    uint32_t crc = crc32(frame.data(), static_cast<size_t>(p - frame.data()));
    *p++ = (crc      ) & 0xFF;
    *p++ = (crc >>  8) & 0xFF;
    *p++ = (crc >> 16) & 0xFF;
    *p++ = (crc >> 24) & 0xFF;

    return frame;
}

} // namespace sdr
