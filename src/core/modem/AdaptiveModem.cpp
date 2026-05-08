#include "sdr/modem/AdaptiveModem.hpp"

namespace sdr {

AdaptiveModem::AdaptiveModem() {
    modem_ = std::make_unique<Modem>(ModScheme::QPSK);
}

ModScheme AdaptiveModem::selectScheme(float snr) const {
    // Use hysteresis: compare against thresholds offset by HYST_UP/DN
    ModScheme cur = modem_->scheme();
    switch (cur) {
        case ModScheme::BPSK:
            if (snr >= SNR_BPSK_QPSK + HYST_UP) return ModScheme::QPSK;
            return ModScheme::BPSK;
        case ModScheme::QPSK:
            if (snr >= SNR_QPSK_16QAM + HYST_UP) return ModScheme::QAM16;
            if (snr <  SNR_BPSK_QPSK  - HYST_DN) return ModScheme::BPSK;
            return ModScheme::QPSK;
        case ModScheme::QAM16:
            if (snr >= SNR_16QAM_64QAM + HYST_UP) return ModScheme::QAM64;
            if (snr <  SNR_QPSK_16QAM  - HYST_DN) return ModScheme::QPSK;
            return ModScheme::QAM16;
        case ModScheme::QAM64:
            if (snr < SNR_16QAM_64QAM - HYST_DN) return ModScheme::QAM16;
            return ModScheme::QAM64;
    }
    return ModScheme::QPSK;
}

void AdaptiveModem::switchTo(ModScheme s) {
    if (s != modem_->scheme())
        modem_ = std::make_unique<Modem>(s);
}

void AdaptiveModem::updateSNR(float snr_db) {
    std::lock_guard<std::mutex> lk(mu_);
    snr_db_ = snr_db;
    switchTo(selectScheme(snr_db));
}

ModScheme AdaptiveModem::currentScheme() const {
    std::lock_guard<std::mutex> lk(mu_);
    return modem_->scheme();
}

ModCode AdaptiveModem::currentModCode() const {
    switch (currentScheme()) {
        case ModScheme::BPSK:  return ModCode::BPSK;
        case ModScheme::QAM16: return ModCode::QAM16;
        case ModScheme::QAM64: return ModCode::QAM64;
        default:               return ModCode::QPSK;
    }
}

void AdaptiveModem::modulate(const uint8_t* bytes, int n,
                              std::vector<std::complex<float>>& iq) {
    std::lock_guard<std::mutex> lk(mu_);
    modem_->modulate(bytes, n, iq);
}

bool AdaptiveModem::demodulate(const std::complex<float>* iq, int n,
                                std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    return modem_->demodulate(iq, n, bytes);
}

int AdaptiveModem::bitsPerSymbol() const {
    std::lock_guard<std::mutex> lk(mu_);
    return modem_->bitsPerSymbol();
}

std::string AdaptiveModem::name() const {
    std::lock_guard<std::mutex> lk(mu_);
    return "AUTO/" + modem_->name();
}

} // namespace sdr
