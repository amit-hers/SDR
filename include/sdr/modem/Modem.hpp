#pragma once
#include "IModem.hpp"
#include <liquid/liquid.h>

namespace sdr {

enum class ModScheme { BPSK, QPSK, QAM16, QAM64 };

// Single fixed-scheme modem wrapping liquid-dsp modemcf.
class Modem : public IModem {
public:
    explicit Modem(ModScheme scheme);
    ~Modem() override;
    Modem(const Modem&)            = delete;
    Modem& operator=(const Modem&) = delete;

    void modulate(const uint8_t* bytes, int n,
                  std::vector<std::complex<float>>& iq) override;
    bool demodulate(const std::complex<float>* iq, int n,
                    std::vector<uint8_t>& bytes) override;

    int         bitsPerSymbol() const override { return bps_; }
    std::string name()          const override { return name_; }
    ModScheme   scheme()        const          { return scheme_; }

    static modulation_scheme toLiquid(ModScheme s);

private:
    ModScheme   scheme_;
    modem       mod_  {nullptr};
    modem       dem_  {nullptr};
    int         bps_  {2};
    std::string name_;
};

} // namespace sdr
