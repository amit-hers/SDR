#pragma once
#include "IMode.hpp"
#include "../Config.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/modem/AdaptiveModem.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/AGC.hpp"
#include "sdr/dsp/TimingSync.hpp"
#include "sdr/dsp/CostasLoop.hpp"
#include "sdr/dsp/FFTSpectrum.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include "sdr/transport/TUNTAPDevice.hpp"
#include "sdr/stats/LinkStats.hpp"
#include <thread>
#include <atomic>
#include <memory>

namespace sdr {

class BridgeMode : public IMode {
public:
    explicit BridgeMode(const Config& cfg, PlutoSDR& radio);
    ~BridgeMode() override;

    void  start()          override;
    void  stop()           override;
    bool  running() const  override { return running_.load(); }
    const LinkStats& stats() const override { return stats_; }

private:
    void txThread();
    void rxThread();
    void statThread();

    const Config& cfg_;
    PlutoSDR&     radio_;
    LinkStats     stats_;

    std::unique_ptr<TUNTAPDevice>  tap_;
    std::unique_ptr<AdaptiveModem> amod_;
    std::unique_ptr<ReedSolomon>   fec_;
    std::unique_ptr<AESCipher>     aes_;
    std::unique_ptr<FFTSpectrum>   fft_;

    std::atomic<bool>  running_{false};
    std::thread        tx_thread_;
    std::thread        rx_thread_;
    std::thread        stat_thread_;
    std::atomic<uint32_t> tx_seq_{0};

    static constexpr size_t IQ_SAMPLES = 256 * 1024;
};

} // namespace sdr
