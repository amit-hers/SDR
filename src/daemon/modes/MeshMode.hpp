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
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include "sdr/transport/TUNTAPDevice.hpp"
#include <thread>
#include <atomic>
#include <memory>

namespace sdr {

// TUN-based L3 mesh mode — routes IP packets over RF
class MeshMode : public IMode {
public:
    explicit MeshMode(const Config& cfg, PlutoSDR& radio);
    ~MeshMode() override;

    void  start()          override;
    void  stop()           override;
    bool  running() const  override { return running_.load(); }
    const LinkStats& stats() const override { return stats_; }

private:
    void txThread();
    void rxThread();

    const Config& cfg_;
    PlutoSDR&     radio_;
    LinkStats     stats_;

    std::unique_ptr<TUNTAPDevice>  tun_;
    std::unique_ptr<AdaptiveModem> amod_;
    std::unique_ptr<ReedSolomon>   fec_;
    std::unique_ptr<AESCipher>     aes_;

    std::atomic<bool>     running_{false};
    std::thread           tx_thread_;
    std::thread           rx_thread_;
    std::atomic<uint32_t> tx_seq_{0};

    static constexpr size_t IQ_SAMPLES = 256 * 1024;
};

} // namespace sdr
