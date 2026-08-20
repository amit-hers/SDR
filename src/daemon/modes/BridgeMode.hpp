#pragma once
#include "IMode.hpp"
#include "../Config.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/ArqWindow.hpp"
#include "sdr/modem/AdaptiveModem.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/AGC.hpp"
#include "sdr/dsp/TimingSync.hpp"
#include "sdr/dsp/CostasLoop.hpp"
#include "sdr/dsp/FFTSpectrum.hpp"
#include "sdr/dsp/CoarseFreqCorrect.hpp"
#include "sdr/dsp/BurstDetector.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include "sdr/transport/TUNTAPDevice.hpp"
#include "sdr/transport/SPSCRing.hpp"
#include "sdr/stats/LinkStats.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <fstream>

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
    std::unique_ptr<ArqWindow>     arq_;

    // RX->TX handoff for outgoing ACK/control frames (fixed size: preamble+
    // header+CRC, zero payload). RX thread must never call radio_.txPush
    // directly.
    SPSCRing<32, WIRE_FRAME_OVERHEAD> ctrl_ring_;

    std::atomic<bool>  running_{false};
    std::thread        tx_thread_;
    std::thread        rx_thread_;
    std::thread        stat_thread_;
    std::atomic<uint32_t> tx_seq_{0};

    // Diagnostic: if $SDR_FRAME_LOG is set, every decoded frame (successful
    // or CRC-failed) is logged there with its content, for manual RF-link
    // analysis. Off by default (empty path -> no file opened).
    std::ofstream frame_log_;

    // Diagnostic: if $SDR_RAW_LOG is set, every raw demodulated byte (before
    // framing) is dumped there as hex, for manually inspecting what the
    // receiver actually produced regardless of whether a frame was found.
    std::ofstream raw_log_;

    static constexpr size_t IQ_SAMPLES = 256 * 1024;

    // Sub-grid alignments to retry demodulation at per detected burst.
    // Matches RRC_TAPS: TimingSync's ability to lock depends on where the
    // window starts relative to the filter's tap grid, so sweeping a full
    // grid period guarantees hitting a workable alignment. See rxThread().
    static constexpr size_t DECODE_OFFSETS = RRC_TAPS;
};

} // namespace sdr
