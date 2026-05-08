#pragma once
#include "IMode.hpp"
#include "../Config.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/modem/Modem.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/AGC.hpp"
#include "sdr/dsp/TimingSync.hpp"
#include "sdr/dsp/CostasLoop.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include "sdr/transport/UDPSocket.hpp"
#include <thread>
#include <atomic>
#include <memory>

namespace sdr {

// Point-to-point mode: single stream via UDP tunnel
// cfg.mode == "p2p-tx" → transmit side, "p2p-rx" → receive side
class P2PMode : public IMode {
public:
    explicit P2PMode(const Config& cfg, PlutoSDR& radio);
    ~P2PMode() override;

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

    std::unique_ptr<Modem>       modem_;
    std::unique_ptr<ReedSolomon> fec_;
    std::unique_ptr<AESCipher>   aes_;
    std::unique_ptr<UDPSocket>   udp_;

    std::atomic<bool>     running_{false};
    std::thread           active_thread_;
    std::atomic<uint32_t> tx_seq_{0};

    static constexpr size_t IQ_SAMPLES = 256 * 1024;
    static constexpr uint16_t P2P_PORT = 5005;
};

} // namespace sdr
