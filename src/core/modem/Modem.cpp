#include "sdr/modem/Modem.hpp"
#include <cstring>
#include <stdexcept>

namespace sdr {

static inline liquid_float_complex to_lc(std::complex<float> c) {
    liquid_float_complex lc;
    std::memcpy(&lc, &c, sizeof(lc));
    return lc;
}
static inline std::complex<float> from_lc(liquid_float_complex lc) {
    std::complex<float> c;
    std::memcpy(&c, &lc, sizeof(c));
    return c;
}

modulation_scheme Modem::toLiquid(ModScheme s) {
    switch (s) {
        case ModScheme::BPSK:  return LIQUID_MODEM_BPSK;
        case ModScheme::QPSK:  return LIQUID_MODEM_QPSK;
        case ModScheme::QAM16: return LIQUID_MODEM_QAM16;
        case ModScheme::QAM64: return LIQUID_MODEM_QAM64;
    }
    return LIQUID_MODEM_QPSK;
}

Modem::Modem(ModScheme scheme) : scheme_(scheme) {
    auto ls = toLiquid(scheme);
    mod_  = modem_create(ls);
    dem_  = modem_create(ls);
    bps_  = modem_get_bps(mod_);
    switch (scheme) {
        case ModScheme::BPSK:  name_ = "BPSK";  break;
        case ModScheme::QPSK:  name_ = "QPSK";  break;
        case ModScheme::QAM16: name_ = "16QAM"; break;
        case ModScheme::QAM64: name_ = "64QAM"; break;
    }
}

Modem::~Modem() {
    if (mod_) modem_destroy(mod_);
    if (dem_) modem_destroy(dem_);
}

void Modem::modulate(const uint8_t* bytes, int n,
                     std::vector<std::complex<float>>& iq) {
    int total_bits = n * 8;
    int n_syms = (total_bits + bps_ - 1) / bps_;
    iq.resize(static_cast<size_t>(n_syms));

    int bit_idx = 0;
    for (int i = 0; i < n_syms; ++i) {
        unsigned sym = 0;
        for (int b = 0; b < bps_; ++b) {
            int byte_i = bit_idx / 8;
            int bit_i  = 7 - (bit_idx % 8);
            unsigned bit = (byte_i < n)
                         ? ((bytes[static_cast<size_t>(byte_i)] >> bit_i) & 1u)
                         : 0u;
            sym = (sym << 1) | bit;
            ++bit_idx;
        }
        liquid_float_complex lout{};
        modem_modulate(mod_, sym, &lout);
        iq[static_cast<size_t>(i)] = from_lc(lout);
    }
}

bool Modem::demodulate(const std::complex<float>* iq, int n,
                        std::vector<uint8_t>& bytes) {
    int total_bits  = n * bps_;
    int total_bytes = (total_bits + 7) / 8;
    bytes.assign(static_cast<size_t>(total_bytes), 0);

    int bit_idx = 0;
    for (int i = 0; i < n; ++i) {
        liquid_float_complex lin = to_lc(iq[static_cast<size_t>(i)]);
        unsigned sym = 0;
        modem_demodulate(dem_, lin, &sym);
        for (int b = bps_ - 1; b >= 0; --b) {
            int byte_i = bit_idx / 8;
            int bit_i  = 7 - (bit_idx % 8);
            if (byte_i < total_bytes)
                bytes[static_cast<size_t>(byte_i)] |=
                    static_cast<uint8_t>(((sym >> b) & 1u) << bit_i);
            ++bit_idx;
        }
    }
    return true;
}

} // namespace sdr
