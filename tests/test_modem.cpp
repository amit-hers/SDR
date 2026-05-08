#include "sdr/modem/Modem.hpp"
#include "sdr/modem/AdaptiveModem.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <complex>

static void test_mod_demod(sdr::ModScheme scheme, const char* name) {
    sdr::Modem modem(scheme);
    std::vector<uint8_t> tx = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

    std::vector<std::complex<float>> iq;
    modem.modulate(tx.data(), static_cast<int>(tx.size()), iq);
    assert(!iq.empty());

    std::vector<uint8_t> rx;
    modem.demodulate(iq.data(), static_cast<int>(iq.size()), rx);
    assert(rx == tx);
    std::cout << "  [modem] " << name << " roundtrip: PASS\n";
}

static void test_adaptive_snr() {
    sdr::AdaptiveModem am;

    // Low SNR → BPSK
    am.updateSNR(3.0f);
    assert(am.currentScheme() == sdr::ModScheme::BPSK);

    // Mid SNR → QPSK (needs to cross 7+2=9 dB upward)
    am.updateSNR(10.0f);
    assert(am.currentScheme() == sdr::ModScheme::QPSK);

    // High SNR → 16QAM (needs 13+2=15 dB)
    am.updateSNR(16.0f);
    assert(am.currentScheme() == sdr::ModScheme::QAM16);

    // Very high → 64QAM (22+2=24 dB)
    am.updateSNR(25.0f);
    assert(am.currentScheme() == sdr::ModScheme::QAM64);

    // Drop but hysteresis keeps 64QAM until 22-1=21 dB
    am.updateSNR(21.5f);
    assert(am.currentScheme() == sdr::ModScheme::QAM64);

    am.updateSNR(20.0f);
    assert(am.currentScheme() == sdr::ModScheme::QAM16);

    // Check ModCode mapping
    assert(am.currentModCode() == sdr::ModCode::QAM16);

    std::cout << "  [modem] adaptive SNR thresholds + hysteresis: PASS\n";
}

void run_modem() {
    std::cout << "[modem tests]\n";
    test_mod_demod(sdr::ModScheme::BPSK,  "BPSK");
    test_mod_demod(sdr::ModScheme::QPSK,  "QPSK");
    test_mod_demod(sdr::ModScheme::QAM16, "16QAM");
    test_mod_demod(sdr::ModScheme::QAM64, "64QAM");
    test_adaptive_snr();
    std::cout << "[modem tests] ALL PASS\n\n";
}
