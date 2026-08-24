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
    state_    = State::HUNT;
    shift_    = 0;
    hdr_pos_  = 0;
    pay_pos_  = 0;
    inverted_ = false;
}

std::optional<DecodedFrame> Deframer::push(uint8_t byte,
                                           const ReedSolomon* fec,
                                           const AESCipher*   aes) {
    switch (state_) {
    // ── HUNT: scan for 4-byte sync word (either phase) ──────────────────
    case State::HUNT:
        shift_ = (shift_ << 8) | byte;
        // Accept the complemented sync word too, and remember that this
        // frame arrived on the inverted carrier phase so the remaining
        // bytes can be corrected as they come in.
        if (shift_ == FRAME_SYNC || shift_ == ~FRAME_SYNC) {
            inverted_ = (shift_ != FRAME_SYNC);
            // Pre-fill header buffer with sync bytes (canonical, un-inverted)
            hdr_buf_[0] = 0xC0; hdr_buf_[1] = 0xFF;
            hdr_buf_[2] = 0xEE; hdr_buf_[3] = 0x77;
            hdr_pos_  = 4;
            state_    = State::HEADER;
        }
        break;

    // ── HEADER: collect remaining 14 bytes (after 4B sync) ──────────────
    case State::HEADER:
        if (inverted_) byte = static_cast<uint8_t>(~byte);
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
        if (inverted_) byte = static_cast<uint8_t>(~byte);
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

            const uint16_t plen_saved  = plen_;
            const uint8_t  flags_saved = flags_;
            const uint32_t seq_saved   = seq_;
            const int      pay_total_saved = pay_total_;
            reset();  // ready for next frame regardless of outcome

            int wire_len = pay_total_saved - static_cast<int>(CRC_SIZE);
            std::vector<uint8_t> wire(payload_buf_.begin(),
                                      payload_buf_.begin() + wire_len);

            // Reed-Solomon runs BEFORE the CRC verdict, not after.
            //
            // This previously rejected on CRC mismatch and only then reached
            // the FEC decode, so RS could never repair anything -- it ran
            // exclusively on frames that were already intact. Every corrected
            // frame it might have rescued was discarded one branch earlier.
            //
            // Framer computes the CRC over the RS-ENCODED codeword (see
            // Framer::encode: FEC first, then CRC over the body), so a
            // repaired frame is verified by re-encoding the corrected payload
            // to reconstruct the codeword as transmitted -- RS encoding is
            // deterministic, so that reproduction is exact -- and checking the
            // CRC against that. The header and the CRC bytes themselves sit
            // outside RS protection (~22 of ~1066 bytes), so errors landing
            // there remain unrecoverable.
            bool crc_ok = (got == expected);
            std::vector<uint8_t> raw;

            if (fec && (flags_saved & FL_FEC)) {
                std::vector<uint8_t> tryw = wire;
                if (aes && (flags_saved & FL_ENCRYPT))
                    aes->crypt(tryw.data(), tryw.size(), static_cast<uint64_t>(seq_saved));
                bool rescued = false;
                try {
                    raw = fec->decode(tryw.data(), tryw.size());
                    if (!crc_ok) {
                        // Re-encode and re-check: did RS restore the exact
                        // codeword the transmitter sent?
                        std::vector<uint8_t> fixed = fec->encode(raw.data(), raw.size());
                        if (fixed.size() == static_cast<size_t>(wire_len)) {
                            std::vector<uint8_t> body2(HEADER_SIZE + fixed.size());
                            std::memcpy(body2.data(), hdr_buf_, HEADER_SIZE);
                            std::memcpy(body2.data() + HEADER_SIZE, fixed.data(), fixed.size());
                            if (crc32(body2.data(), body2.size()) == got) {
                                crc_ok  = true;
                                rescued = true;
                            }
                        }
                    }
                } catch (...) {
                    if (!crc_ok) { ++crc_errors_; return std::nullopt; }
                }
                if (!crc_ok) { ++crc_errors_; return std::nullopt; }
                if (rescued) ++fec_rescued_;
                raw.resize(plen_saved);
            } else {
                if (!crc_ok) { ++crc_errors_; return std::nullopt; }
                if (aes && (flags_saved & FL_ENCRYPT))
                    aes->crypt(wire.data(), wire.size(), static_cast<uint64_t>(seq_saved));
                raw = std::move(wire);
                raw.resize(plen_saved);
            }

            ++good_frames_;
            DecodedFrame df;
            df.payload  = std::move(raw);
            df.flags    = flags_saved;
            df.mod      = mod_;
            df.bw       = bw_;
            df.node_id  = node_id_;
            df.seq      = seq_saved;
            return df;
        }
        break;
    }
    return std::nullopt;
}

} // namespace sdr
