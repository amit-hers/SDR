#pragma once
#include <vector>
#include <complex>
#include <string>
#include <cstdint>

namespace sdr {

class IModem {
public:
    virtual ~IModem() = default;

    // Modulate raw bytes into IQ symbols.
    virtual void modulate(const uint8_t* bytes, int n,
                          std::vector<std::complex<float>>& iq) = 0;

    // Demodulate IQ symbols into bytes. Returns false if symbol count is wrong.
    virtual bool demodulate(const std::complex<float>* iq, int n,
                            std::vector<uint8_t>& bytes) = 0;

    virtual int         bitsPerSymbol() const = 0;
    virtual std::string name()          const = 0;

    // Number of IQ symbols needed to carry n bytes.
    int symbolsForBytes(int n) const {
        int bits = n * 8;
        return (bits + bitsPerSymbol() - 1) / bitsPerSymbol();
    }
};

} // namespace sdr
