#pragma once
#include "IMode.hpp"
#include "../Config.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/ArqWindow.hpp"
#include "sdr/framing/Aggregate.hpp"
#include "sdr/modem/SplitModem.hpp"
#include "sdr/phy/SoftwarePhy.hpp"
#include "sdr/dsp/RRCFilter.hpp"
#include "sdr/dsp/AGC.hpp"
#include "sdr/dsp/TimingSync.hpp"
#include "sdr/dsp/CostasLoop.hpp"
#include "sdr/dsp/FFTSpectrum.hpp"
#include "sdr/dsp/CoarseFreqCorrect.hpp"
#include "sdr/dsp/BurstDetector.hpp"
#include "sdr/dsp/PreambleSync.hpp"
#include "sdr/dsp/DataAidedSync.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/crypto/AESCipher.hpp"
#include "sdr/transport/TUNTAPDevice.hpp"
#include "sdr/transport/SPSCRing.hpp"
#include "sdr/stats/LinkStats.hpp"
#include "sdr/stats/StageProfiler.hpp"
#include <array>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <fstream>
#include "sdr/dsp/FixedTimingSync.hpp"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <vector>

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
    // Drains the TAP continuously into tap_queue_, so that ingestion is never
    // blocked by the transmit worker's duty-limit sleeps.
    //
    // txThread previously read the TAP, aggregated, modulated, pushed AND
    // slept for the duty limiter all on one thread. It spent ~24% of wall
    // time asleep enforcing duty, during which nothing drained the TAP, so
    // offered traffic backed up and was dropped: at 600 pkt/s offered only
    // ~160 pkt/s were ever read, and TX duty sat pinned near 49% regardless
    // of load because the frame rate -- not the radio -- was the limit.
    //
    // Mirrors the receive side, where captureThread already feeds rxThread
    // through a bounded queue for the same reason.
    void tapReaderThread();
    // Drains modulated sample buffers to the radio.
    //
    // txPush blocks until the radio accepts the samples -- measured at 50.3%
    // of the transmit worker's wall time, during which it modulated nothing.
    // Splitting the push onto its own thread lets modulation of the next
    // buffer overlap transmission of the current one. Duty pacing lives here
    // too, since it is a property of when samples reach the air.

    void captureThread();   // does nothing but rxPull -> queue, so reception never stops
    void rxThread();        // consumes the queue and runs the DSP chain
    void statThread();
    void applyRealtime(const char* who, int core);

    const Config& cfg_;
    PlutoSDR&     radio_;
    LinkStats     stats_;

    // Stage-level CPU accounting, enabled by $SDR_PROFILE. Gates any
    // optimisation work: without it, "where the CPU goes" is guesswork.
    StageProfiler prof_;
    std::chrono::steady_clock::time_point prof_t0_;
    double        prof_cpu0_{0.0};

    // The physical layer, behind the DTC boundary. Today this is SoftwarePhy,
    // which streams IQ and runs the PHY on the host; a RadioPhy speaking
    // DtcProtocol replaces it without this class changing. Everything the
    // daemon keeps -- TAP/IP, aggregation, ARQ, routing, link adaptation --
    // is per-frame work that belongs on the host whichever radio is attached.
    std::unique_ptr<SoftwarePhy>   phy_;

    std::unique_ptr<TUNTAPDevice>  tap_;
    std::unique_ptr<ReedSolomon>   fec_;
    std::unique_ptr<AESCipher>     aes_;
    std::unique_ptr<ArqWindow>     arq_;

    // RX->TX handoff for outgoing ACK/control frames (fixed size: preamble+
    // header+CRC, zero payload). RX thread must never call radio_.txPush
    // directly.
    // Sequence numbers the receive path wants acknowledged. It used to carry
    // fully-encoded ACK frames; with framing inside the PHY the daemon has no
    // business building wire bytes, so it passes the seq and lets the PHY
    // frame it.
    SPSCRing<32, sizeof(uint32_t)> ctrl_ring_;

    // ── Modulation state ──────────────────────────────────────────────────
    // TX and RX modulation are deliberately *separate*. They used to share
    // one AdaptiveModem, whose scheme was driven by this node's own RX SNR --
    // so the receiver's local signal quality silently changed what the
    // transmitter emitted, and two nodes adapting independently could never
    // agree. Worse, the acquisition section inherited that scheme, which the
    // BPSK-only PreambleSync could not correlate against at all.
    //
    // tx_mod_ is configured, fixed, and never touched by the RX path. The
    // receive side takes its payload modulation from each frame's header
    // (SplitModem::Result::payload_mod) and has no persistent state at all.
    ModCode tx_mod_{ModCode::BPSK};


    // Aggregation timing: how long a partly-filled aggregate waits before it
    // is sealed, and how long a sealed payload waits before the PHY is told
    // to flush. Both are daemon-side latency policy.
    static constexpr int TX_LATENCY_MS   = 8;
    static constexpr int TX_AGGREGATE_MS = 4;

    // ── What the daemon itself owns ──────────────────────────────────────
    // TAP ingress, aggregation and ARQ. Everything below the boundary is
    // accounted for by the PHY, which reports it separately.
    std::atomic<uint64_t> tx_tap_pkts_{0};
    std::atomic<uint64_t> tx_tap_bytes_{0};
    std::atomic<uint64_t> tx_tap_empty_{0};     // polls that found the TAP queue empty
    std::atomic<uint64_t> tx_frames_sealed_{0}; // aggregates closed into a payload
    std::atomic<uint64_t> tx_arq_blocked_{0};   // held because the ARQ window was full
    // TAP delivery on receive, from the onFrame handler.
    std::atomic<uint64_t> rx_records_{0};
    std::atomic<uint64_t> rx_rec_written_{0};
    std::atomic<uint64_t> rx_rec_failed_{0};

    std::atomic<bool>  running_{false};
    std::thread        tx_thread_;
    std::thread        tap_reader_thread_;
    // Bounded so a stalled transmitter cannot grow it without limit; dropping
    // the oldest packet is better than unbounded latency on a live datalink.
    std::deque<std::vector<uint8_t>> tap_queue_;
    std::mutex              tap_mu_;
    std::condition_variable tap_cv_;
    std::atomic<uint64_t>   tap_q_drops_{0};
    std::atomic<uint64_t>   tap_q_hiwater_{0};
    static constexpr size_t TAP_QUEUE_MAX = 2048;

    // Modulated buffers awaiting transmission. Deliberately shallow: two in
    // flight is enough to keep the radio fed while the next is prepared, and
    // more would only add latency ahead of the air.
    std::mutex              tx_sq_mu_;
    std::condition_variable tx_sq_cv_;
    std::atomic<uint64_t>   tx_sq_stalls_{0};    // worker waited for room
    static constexpr size_t TX_SAMPLE_Q_MAX = 3;
    std::thread        stat_thread_;
    std::atomic<uint32_t> tx_seq_{0};
    // Samples the DAC conversion had to clamp. Should stay at zero; see
    // TX_PEAK_OVERSHOOT in the .cpp.










    // NOTE: the old brute-force alignment sweep (retrying the whole DSP
    // chain at every RRC_TAPS offset) has been replaced by PreambleSync,
    // which correlates against the preamble+sync-word to compute the frame
    // start directly. See rxThread().
};

} // namespace sdr
