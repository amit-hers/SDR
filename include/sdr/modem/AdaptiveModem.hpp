#pragma once
#include "IModem.hpp"
#include "Modem.hpp"
#include "sdr/framing/Frame.hpp"   // for ModCode
#include <memory>
#include <mutex>

namespace sdr {

// Wraps a Modem and automatically switches modulation scheme based on SNR.
// Hysteresis: 2 dB to step up, 1 dB to step down (avoids rapid toggling).
class AdaptiveModem : public IModem {
public:
    AdaptiveModem();

    void modulate(const uint8_t* bytes, int n,
                  std::vector<std::complex<float>>& iq) override;
    bool demodulate(const std::complex<float>* iq, int n,
                    std::vector<uint8_t>& bytes) override;

    int         bitsPerSymbol() const override;
    std::string name()          const override;

    // Called from RX thread after each frame; triggers scheme switch if needed.
    void updateSNR(float snr_db);

    ModScheme currentScheme()  const;
    ModCode   currentModCode() const;  // maps ModScheme → ModCode (1=BPSK...4=64QAM)

    // SNR thresholds (centre values; hysteresis applied around them)
    static constexpr float SNR_BPSK_QPSK   =  7.f;
    static constexpr float SNR_QPSK_16QAM  = 13.f;
    static constexpr float SNR_16QAM_64QAM = 22.f;
    static constexpr float HYST_UP         =  2.f;
    static constexpr float HYST_DN         =  1.f;

private:
    mutable std::mutex    mu_;
    std::unique_ptr<Modem> modem_;
    float snr_db_{0.f};

    ModScheme selectScheme(float snr) const;
    void      switchTo(ModScheme s);
};

} // namespace sdr
