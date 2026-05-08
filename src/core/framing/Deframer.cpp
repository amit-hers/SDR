#include "sdr/framing/Deframer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include <cstring>
#include <stdexcept>

namespace sdr {

static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

static uint32_t get_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}
static uint16_t get_u16_be(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

void Deframer::reset() {
    state_   = State::HUNT;
    shift_   = 0;
    hdr_pos_ = 0;
    pay_pos_ = 0;
}

std::optional<DecodedFrame> Deframer::push(uint8_t byte,
                                           const ReedSolomon* fec,
                                           const AESCipher*   aes) {
    switch (state_) {
    // ── HUNT: scan for 4-byte sync word ─────────────────────────────────
    case State::HUNT:
        shift_ = (shift_ << 8) | byte;
        if (shift_ == FRAME_SYNC) {
            // Pre-fill header buffer with sync bytes
            hdr_buf_[0] = 0xC0; hdr_buf_[1] = 0xFF;
            hdr_buf_[2] = 0xEE; hdr_buf_[3] = 0x77;
            hdr_pos_  = 4;
            state_    = State::HEADER;
        }
        break;

    // ── HEADER: collect remaining 14 bytes (after 4B sync) ──────────────
    case State::HEADER:
        hdr_buf_[hdr_pos_++] = byte;
        if (hdr_pos_ == static_cast<int>(HEADER_SIZE)) {
            // Parse header
            const uint8_t* h = hdr_buf_;
            // h[0..3] = sync (already validated)
            // h[4] = ver, h[5] = flags, h[6] = mod, h[7] = bw
            flags_   = h[5];
            mod_     = static_cast<ModCode>(h[6]);
            bw_      = static_cast<BwCode>(h[7]);
            node_id_ = get_u32_be(h + 8);
            seq_     = get_u32_be(h + 12);
            plen_    = get_u16_be(h + 16);  // original (pre-FEC) payload length

            if (plen_ > MAX_PAYLOAD) {
                reset(); break;             // malformed, re-hunt
            }

            // How many bytes to collect: FEC-encoded size + 4B CRC
            int wire_payload = static_cast<int>(plen_);
            if (fec && (flags_ & FL_FEC))
                wire_payload = static_cast<int>(ReedSolomon::encodedSize(plen_));

            pay_total_ = wire_payload + static_cast<int>(CRC_SIZE);
            pay_pos_   = 0;
            state_     = State::PAYLOAD;
        }
        break;

    // ── PAYLOAD: collect payload + CRC ──────────────────────────────────
    case State::PAYLOAD:
        payload_buf_[pay_pos_++] = byte;
        if (pay_pos_ == pay_total_) {
            // Verify CRC over header + payload (excluding the 4 CRC bytes)
            size_t body_len = HEADER_SIZE + static_cast<size_t>(pay_total_ - 4);
            uint8_t  body[HEADER_SIZE + MAX_PAYLOAD + 255];
            std::memcpy(body, hdr_buf_, HEADER_SIZE);
            std::memcpy(body + HEADER_SIZE, payload_buf_.data(),
                        static_cast<size_t>(pay_total_ - 4));

            uint32_t expected = crc32(body, body_len);
            uint32_t got = uint32_t(payload_buf_[pay_total_-4])
                        | (uint32_t(payload_buf_[pay_total_-3]) <<  8)
                        | (uint32_t(payload_buf_[pay_total_-2]) << 16)
                        | (uint32_t(payload_buf_[pay_total_-1]) << 24);

            reset();  // ready for next frame regardless of outcome

            if (got != expected) {
                ++crc_errors_;
                return std::nullopt;
            }

            // CRC OK — decrypt then FEC-decode
            int wire_len = pay_total_ - static_cast<int>(CRC_SIZE);
            std::vector<uint8_t> wire(payload_buf_.begin(),
                                      payload_buf_.begin() + wire_len);

            if (aes && (flags_ & FL_ENCRYPT))
                aes->crypt(wire.data(), wire.size(), static_cast<uint64_t>(seq_));

            std::vector<uint8_t> raw;
            if (fec && (flags_ & FL_FEC)) {
                try {
                    raw = fec->decode(wire.data(), wire.size());
                } catch (...) {
                    ++crc_errors_;
                    return std::nullopt;
                }
                raw.resize(plen_);  // trim to original length
            } else {
                raw = std::move(wire);
                raw.resize(plen_);
            }

            ++good_frames_;
            DecodedFrame df;
            df.payload  = std::move(raw);
            df.flags    = flags_;
            df.mod      = mod_;
            df.bw       = bw_;
            df.node_id  = node_id_;
            df.seq      = seq_;
            return df;
        }
        break;
    }
    return std::nullopt;
}

} // namespace sdr
