#include "sdr/crypto/AESCipher.hpp"
#include <openssl/evp.h>
#include <openssl/err.h>
#include <cstring>
#include <stdexcept>

namespace sdr {

AESCipher::AESCipher(const uint8_t* key) {
    setKey(key);
}

void AESCipher::setKey(const uint8_t* key) {
    std::memcpy(key_.data(), key, KEY_SIZE);
    has_key_ = true;
}

void AESCipher::crypt(uint8_t* data, size_t len, uint64_t ctr) const {
    if (!has_key_)
        throw std::logic_error("AESCipher: no key set");

    // 128-bit IV: 8 zero bytes || 64-bit counter big-endian
    uint8_t iv[16]{};
    iv[ 8] = (ctr >> 56) & 0xFF;
    iv[ 9] = (ctr >> 48) & 0xFF;
    iv[10] = (ctr >> 40) & 0xFF;
    iv[11] = (ctr >> 32) & 0xFF;
    iv[12] = (ctr >> 24) & 0xFF;
    iv[13] = (ctr >> 16) & 0xFF;
    iv[14] = (ctr >>  8) & 0xFF;
    iv[15] =  ctr        & 0xFF;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("AESCipher: EVP_CIPHER_CTX_new failed");

    int ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr,
                                key_.data(), iv);
    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AESCipher: EVP_EncryptInit_ex failed");
    }

    int out_len = 0;
    EVP_EncryptUpdate(ctx, data, &out_len, data, static_cast<int>(len));
    int final_len = 0;
    EVP_EncryptFinal_ex(ctx, data + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);
}

} // namespace sdr
