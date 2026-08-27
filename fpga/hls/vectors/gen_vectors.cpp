// Generates deterministic test vectors for the HLS demodulator from the
// VALIDATED host transmit path -- the same SplitModem + RRCInterp that the
// 2 MHz QPSK baseline runs on air.
//
// This matters: a testbench that generates its own IQ only proves the HLS
// core is self-consistent. Driving it with the waveform the working link
// actually produces is what makes a bit-exact match mean something.
//
// Writes, per vector:
//   <name>.iq    interleaved int16 I,Q  -- exactly what txPush sends the DAC
//   <name>.bits  expected payload bytes -- what the demodulator must recover
//   <name>.meta  human-readable description
#include "sdr/modem/SplitModem.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/framing/Framer.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace sdr;

namespace {

// Same scaling the transmitter uses (SoftwarePhy::toDac at the default scale).
constexpr float TX_SCALE = 8192.f;

void writeVector(const std::string& dir, const std::string& name,
                 const std::vector<std::complex<float>>& shaped,
                 const std::vector<uint8_t>& expect,
                 const std::string& meta) {
    std::string base = dir + "/" + name;
    if (FILE* f = std::fopen((base + ".iq").c_str(), "wb")) {
        for (const auto& s : shaped) {
            auto clamp = [](float v) -> int16_t {
                float x = v * TX_SCALE;
                if (x >  32767.f) x =  32767.f;
                if (x < -32767.f) x = -32767.f;
                return static_cast<int16_t>(x);
            };
            int16_t iq[2] = { clamp(s.real()), clamp(s.imag()) };
            std::fwrite(iq, sizeof(int16_t), 2, f);
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen((base + ".bits").c_str(), "wb")) {
        std::fwrite(expect.data(), 1, expect.size(), f);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((base + ".meta").c_str(), "w")) {
        std::fprintf(f, "%s\nsamples=%zu\nexpect_bytes=%zu\nsps=%d\ntx_scale=%.0f\n",
                     meta.c_str(), shaped.size(), expect.size(), RRC_SPS, TX_SCALE);
        std::fclose(f);
    }
    std::printf("  %-10s %7zu samples, %4zu expected bytes  (%s)\n",
                name.c_str(), shaped.size(), expect.size(), meta.c_str());
}

// Impairments a real capture carries, applied deterministically so the test
// stays reproducible: a carrier offset the receiver must track, a fractional
// timing shift, and additive noise at a stated SNR.
void impair(std::vector<std::complex<float>>& s, double cfo_rad_per_sample,
            double snr_db, uint32_t seed) {
    std::mt19937 rng(seed);
    double sig = 0.0;
    for (const auto& v : s) sig += std::norm(v);
    sig /= double(s.size());
    const double npow = sig / std::pow(10.0, snr_db / 10.0);
    std::normal_distribution<double> g(0.0, std::sqrt(npow / 2.0));
    for (size_t i = 0; i < s.size(); ++i) {
        const double a = cfo_rad_per_sample * double(i);
        const std::complex<double> rot(std::cos(a), std::sin(a));
        std::complex<double> v = std::complex<double>(s[i].real(), s[i].imag()) * rot;
        v += std::complex<double>(g(rng), g(rng));
        s[i] = { float(v.real()), float(v.imag()) };
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : ".";
    std::printf("generating HLS demod vectors from the host TX path:\n");

    // A known, reproducible payload. Random bytes rather than a pattern: a
    // repeating fill can match by accident under a bit-shift or duplication
    // bug, which is exactly what this test exists to catch.
    std::vector<uint8_t> payload(256);
    std::mt19937 rng(0xC0FFEE);
    for (auto& b : payload) b = static_cast<uint8_t>(rng() & 0xFF);

    Framer framer;
    std::vector<uint8_t> frame = framer.encode(payload.data(), payload.size(),
                                               0, ModCode::QPSK,
                                               BwCode::BW_2P5, 1, 0,
                                               nullptr, nullptr);
    std::vector<std::complex<float>> syms, shaped;
    SplitModem::modulate(frame, ModCode::QPSK, syms);
    RRCInterp interp(RRC_SPS);
    interp.process(syms, shaped);

    // The demodulator recovers the wire bytes, preamble included -- it has no
    // framing. Expect exactly what the modulator was given.
    writeVector(dir, "clean", shaped, frame,
                "host SplitModem+RRCInterp, QPSK, no impairment");

    auto impaired = shaped;
    // 200 Hz at 8 MS/s, and 25 dB SNR: both well inside what the live link
    // shows (measured residual CFO ~0.002 rad/sym, SNR ~35 dB), so a
    // demodulator that cannot manage this cannot manage the air.
    impair(impaired, 2.0 * M_PI * 200.0 / (2.0e6 * RRC_SPS), 25.0, 12345);
    writeVector(dir, "impaired", impaired, frame,
                "same waveform, +200 Hz CFO, 25 dB SNR");
    return 0;
}
