#include "sdr/fec/ReedSolomon.hpp"
#include <stdexcept>
#include <cstring>

namespace sdr {

ReedSolomon::ReedSolomon() {
    enc_ = fec_create(LIQUID_FEC_RS_M8, nullptr);
    dec_ = fec_create(LIQUID_FEC_RS_M8, nullptr);
    if (!enc_ || !dec_)
        throw std::runtime_error("ReedSolomon: fec_create failed");
}

ReedSolomon::~ReedSolomon() {
    if (enc_) fec_destroy(enc_);
    if (dec_) fec_destroy(dec_);
}

size_t ReedSolomon::encodedSize(size_t raw_len) {
    size_t blocks = (raw_len + BLOCK_IN - 1) / BLOCK_IN;
    return blocks * BLOCK_OUT;
}

size_t ReedSolomon::decodedSize(size_t enc_len) {
    if (enc_len % BLOCK_OUT != 0)
        throw std::invalid_argument("ReedSolomon: encoded length not multiple of 255");
    return (enc_len / BLOCK_OUT) * BLOCK_IN;
}

std::vector<uint8_t> ReedSolomon::encode(const uint8_t* data, size_t len) const {
    size_t blocks  = (len + BLOCK_IN - 1) / BLOCK_IN;
    std::vector<uint8_t> out(blocks * BLOCK_OUT, 0);

    for (size_t b = 0; b < blocks; ++b) {
        uint8_t in_block[BLOCK_IN]{};
        size_t  offset  = b * static_cast<size_t>(BLOCK_IN);
        size_t  to_copy = (len - offset < static_cast<size_t>(BLOCK_IN))
                        ? (len - offset)
                        : static_cast<size_t>(BLOCK_IN);
        std::memcpy(in_block, data + offset, to_copy);
        fec_encode(enc_, BLOCK_IN, in_block,
                   out.data() + b * static_cast<size_t>(BLOCK_OUT));
    }
    return out;
}

std::vector<uint8_t> ReedSolomon::decode(const uint8_t* data, size_t len) const {
    if (len % static_cast<size_t>(BLOCK_OUT) != 0)
        throw std::invalid_argument("ReedSolomon::decode: length not multiple of 255");

    size_t blocks = len / static_cast<size_t>(BLOCK_OUT);
    std::vector<uint8_t> out(blocks * static_cast<size_t>(BLOCK_IN));

    for (size_t b = 0; b < blocks; ++b) {
        uint8_t out_block[BLOCK_IN]{};
        // fec_decode takes non-const; data is only read, const_cast is safe
        int rc = fec_decode(dec_, BLOCK_IN,
                            const_cast<uint8_t*>(data + b * static_cast<size_t>(BLOCK_OUT)),
                            out_block);
        if (rc != 0)
            throw std::runtime_error("ReedSolomon: uncorrectable error in block "
                                     + std::to_string(b));
        std::memcpy(out.data() + b * static_cast<size_t>(BLOCK_IN),
                    out_block, BLOCK_IN);
    }
    return out;
}

} // namespace sdr
