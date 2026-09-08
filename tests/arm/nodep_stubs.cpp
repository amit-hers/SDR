// Framer and Deframer reference Reed-Solomon and AES symbols even on paths that
// pass nullptr for both, so a link needs them present. Pulling in the real ones
// drags liquid-dsp and OpenSSL across to the target architecture, which is a
// packaging question and not what the wire-format vectors are testing.
//
// These abort rather than return something plausible: if a code path ever
// reaches them the vectors would be silently wrong, and a wrong reference
// vector is worse than a link error.
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void nope(const char* what) {
    std::fprintf(stderr, "wire_vectors: %s was called; these vectors use neither\n", what);
    std::abort();
}
} // namespace

namespace sdr {
ReedSolomon::ReedSolomon()  : enc_(nullptr), dec_(nullptr) {}
ReedSolomon::~ReedSolomon() = default;
std::vector<uint8_t> ReedSolomon::encode(const uint8_t*, size_t) const { nope("ReedSolomon::encode"); }
std::vector<uint8_t> ReedSolomon::decode(const uint8_t*, size_t) const { nope("ReedSolomon::decode"); }
size_t ReedSolomon::encodedSize(size_t raw) { return ((raw + BLOCK_IN - 1) / BLOCK_IN) * BLOCK_OUT; }
size_t ReedSolomon::decodedSize(size_t enc) { return (enc / BLOCK_OUT) * BLOCK_IN; }
void AESCipher::crypt(uint8_t*, size_t, uint64_t) const { nope("AESCipher::crypt"); }
} // namespace sdr
