// Reproducible symbol-domain characterization. This is NOT an RF qualification.
#include "sdr/modem/SplitModem.hpp"
#include "sdr/modem/Modem.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    try {
        unsigned frames=100;
        if(argc==2 && std::string(argv[1])=="--rtl-vectors") {
            sdr::Modem modem(sdr::ModScheme::QAM64);
            for(unsigned label=0;label<64;++label) {
                const uint8_t byte=static_cast<uint8_t>(label<<2);
                std::vector<std::complex<float>> symbols;
                modem.modulate(&byte,1,symbols);
                const auto i=static_cast<uint16_t>(static_cast<int16_t>(std::lround(symbols[0].real()*8192)));
                const auto q=static_cast<uint16_t>(static_cast<int16_t>(std::lround(symbols[0].imag()*8192)));
                std::cout<<std::hex<<std::setfill('0')<<std::setw(4)<<i<<std::setw(4)<<q<<'\n';
            }
            return 0;
        }
        if(argc==2 && std::string(argv[1])=="--help") {
            std::cout<<"sdr-modem-characterize [frames-per-point]\n"
                         "Deterministic AWGN simulation, seed 640016; CSV on stdout.\n";
            return 0;
        }
        if(argc>2) throw std::invalid_argument("expected frame count only");
        if(argc==2) { size_t n=0; auto count=std::stoul(argv[1],&n);
            if(n!=std::string(argv[1]).size() || count<1 || count>100000) throw std::invalid_argument("frame count 1..100000");
            frames=static_cast<unsigned>(count);
        }
        std::cout<<"environment,mode,fec,snr_db,frames,per,raw_ber,evm_rms,clipped_fraction\n";
        // Fixed 64-QAM characterization precedes 16-QAM in the development plan.
        for(auto mode:{sdr::ModCode::BPSK,sdr::ModCode::QPSK,sdr::ModCode::QAM64,sdr::ModCode::QAM16}) {
            for(bool use_fec:{false,true}) for(int snr:{6,12,18,24,30,36}) {
                std::mt19937 rng(640016);
                std::normal_distribution<float> noise(0.f,static_cast<float>(std::sqrt(std::pow(10.,-snr/10.)/2)));
                sdr::ReedSolomon fec;
                uint64_t bad=0,errors=0,bits=0,clipped=0,samples=0;
                double error_power=0,reference_power=0;
                for(unsigned f=0;f<frames;++f) {
                    std::vector<uint8_t> payload(223);
                    for(auto& b:payload)b=static_cast<uint8_t>(rng());
                    auto wire=sdr::Framer().encode(payload.data(),payload.size(),use_fec?sdr::FL_FEC:0,
                                        mode,sdr::BwCode::BW_1P25,1,f,use_fec?&fec:nullptr,nullptr);
                    std::vector<std::complex<float>> symbols;
                    sdr::SplitModem::modulate(wire,mode,symbols);
                    for(auto& s:symbols) {
                        const auto reference=s;
                        s+=std::complex<float>(noise(rng),noise(rng));
                        const float re=std::clamp(s.real(),-4.f,32767.f/8192.f);
                        const float im=std::clamp(s.imag(),-4.f,32767.f/8192.f);
                        clipped+=re!=s.real() || im!=s.imag();
                        s={re,im};
                        error_power+=std::norm(s-reference);
                        reference_power+=std::norm(reference);
                        ++samples;
                    }
                    auto result=sdr::SplitModem::demodulate(symbols,use_fec);
                    std::optional<sdr::DecodedFrame> decoded;
                    if(result.complete) {
                        sdr::Deframer deframer;
                        const auto n=std::min(result.bytes.size(),wire.size()-sdr::PREAMBLE_LEN);
                        for(size_t i=0;i<n;++i) {
                            errors+=std::popcount(unsigned(result.bytes[i]^wire[sdr::PREAMBLE_LEN+i]));bits+=8;
                        }
                        for(auto b:result.bytes)if(auto d=deframer.push(b,use_fec?&fec:nullptr,nullptr))decoded=std::move(d);
                    }
                    if(!decoded || decoded->payload!=payload)++bad;
                }
                if(snr==36 && bad) throw std::runtime_error("high-SNR roundtrip regression");
                std::cout<<"symbol_awgn,"<<sdr::modCodeName(mode)<<','<<(use_fec?"rs255223":"none")<<','
                         <<snr<<','<<frames<<','<<double(bad)/frames<<',';
                if(bits)std::cout<<double(errors)/double(bits);else std::cout<<"unavailable";
                std::cout<<','<<std::sqrt(error_power/reference_power)<<','<<double(clipped)/double(samples)<<'\n';
            }
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
