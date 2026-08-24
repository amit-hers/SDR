#pragma once
#include "IMode.hpp"
#include "../Config.hpp"
#include "sdr/hardware/PlutoSDR.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/ArqWindow.hpp"
#include "sdr/framing/Aggregate.hpp"
#include "sdr/modem/SplitModem.hpp"
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

    std::unique_ptr<TUNTAPDevice>  tap_;
    std::unique_ptr<ReedSolomon>   fec_;
    std::unique_ptr<AESCipher>     aes_;
    std::unique_ptr<FFTSpectrum>   fft_;
    std::unique_ptr<ArqWindow>     arq_;

    // RX->TX handoff for outgoing ACK/control frames (fixed size: preamble+
    // header+CRC, zero payload). RX thread must never call radio_.txPush
    // directly.
    SPSCRing<32, WIRE_FRAME_OVERHEAD> ctrl_ring_;

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

    // Carrier-recovery method for the live RX path, from $SDR_RX_CARRIER.
    // Kept selectable so the Costas-vs-LS comparison can be re-run on real
    // hardware rather than only offline.
    enum class CarrierMode { LS, COSTAS, LS_COSTAS, NONE };
    CarrierMode carrier_mode_{CarrierMode::LS_COSTAS};

    // How far past the aligned start the acquisition section may actually
    // begin. PreambleSync computes the burst start from a correlation peak,
    // and TimingSync's own group delay and settling push the first usable
    // symbol later still: measured on the live link, the sync word lands at
    // symbol ~344, i.e. the preamble starts at ~88, not 0.
    //
    // This bound was 64, which is *below* that. The consequence was subtle
    // rather than fatal: the preamble is 0xAA, period-2 in BPSK, so the
    // correlator could still lock at the edge of its range on an aliased
    // offset and return a plausible-looking estimate anchored to the wrong
    // symbol. Cover the real range instead of relying on that.
    static constexpr size_t PREAMBLE_START_SLACK = 256;

    // Symbols searched for the acquisition reference (preamble + sync word).
    static constexpr size_t DAS_SEARCH_SYMS = PREAMBLE_START_SLACK;

    // Symbols searched for the *sync word*. A different bound: the sync word
    // does not sit at the frame start, it sits one whole preamble later
    // (PREAMBLE_LEN*8 symbols at 1 bit/symbol). Passing the DAS bound here
    // instead puts the sync word outside the window and no frame is ever
    // found -- which looks exactly like a dead link.
    static constexpr size_t SYNC_SEARCH_SYMS =
        PREAMBLE_START_SLACK + PREAMBLE_LEN * 8;

    // How long staged frames may wait for company before being transmitted.
    //
    // Each push carries fixed per-transfer overhead, so batching several
    // frames into one is worth a little delay -- but only a little, since
    // this is added latency on every packet. The old code flushed on every
    // idle TAP poll (once per millisecond), which meant almost no batching
    // at all: measured 6470 samples per push into a 65536-sample buffer.
    static constexpr int TX_AGGREGATE_MS = 2;
    std::chrono::steady_clock::time_point stage_started_{};

    // Alignment attempts per located preamble. PreambleSync's correlation
    // peak used to land a sample or two off what TimingSync wanted, so each
    // burst was retried at base, base-1, base+1 -- and TimingSync, the most
    // expensive stage, runs once per attempt.
    //
    // Measured once the CFO estimate was properly conditioned: the two
    // neighbours together produced 34 of 1289 decodes (2.6%) while inflating
    // TimingSync and AGC call counts by ~30%. Set to 1; raise to 3 to restore
    // the sweep if a future change makes the peak less reliable again.
    static constexpr size_t ALIGN_OFFSETS = 1;

    // Carrier sense. The capture thread publishes "peer heard until"; the TX
    // thread defers while that is in the future. Set from the capture thread
    // rather than the DSP thread because the DSP queue can be up to
    // CAPTURE_QUEUE_MAX buffers deep, which would make the signal stale by
    // several hundred milliseconds -- far longer than a burst.
    std::atomic<int64_t>  peer_busy_until_us_{0};
    std::atomic<uint64_t> cs_defers_{0};
    std::atomic<uint64_t> cs_overrides_{0};
    double                cs_noise_floor_{0.0};   // capture thread only

    // Airtime actually radiated, for verifying the duty limiter.
    std::atomic<double> tx_air_seconds_{0.0};

    // Frames modulated and staged but lost to a short or failed hardware
    // push -- i.e. never radiated. Distinct from frames_tx, which now counts
    // only frames the radio actually accepted.
    std::atomic<uint64_t> tx_frames_lost_{0};

    // TX-path stage counters. Under overload the link collapses rather than
    // plateauing (1600 kbps offered delivered 10 kbps, with channel occupancy
    // FALLING from 44% to 14% -- the transmitter went quieter as it was asked
    // for more). These localise which stage sheds the load.
    std::atomic<uint64_t> tx_tap_pkts_{0};      // packets read off the TAP
    std::atomic<uint64_t> tx_tap_bytes_{0};
    std::atomic<uint64_t> tx_tap_empty_{0};     // polls that found nothing
    std::atomic<uint64_t> tx_frames_sealed_{0}; // aggregates turned into frames
    std::atomic<uint64_t> tx_frames_staged_{0}; // frames modulated into `stage`
    std::atomic<uint64_t> tx_duty_defers_{0};   // flush skipped: duty gap open
    std::atomic<uint64_t> tx_duty_waits_us_{0}; // time actually slept for duty
    std::atomic<uint64_t> tx_stage_forces_{0};  // forced flush: stage would overflow
    std::atomic<uint64_t> tx_pushes_ok_{0};     // txPush completed in full
    std::atomic<uint64_t> tx_pushes_short_{0};  // txPush short/failed
    std::atomic<uint64_t> tx_arq_blocked_{0};

    // Multi-frame walk inside one detected burst window. Node A aggregates
    // ~2 frames per burst; node B was recovering ~0.83, and the decoded frame
    // rate stayed ~55/s across a 2x bandwidth change -- a per-burst limit, not
    // a channel one.
    std::atomic<uint64_t> walk_iters_{0};
    std::atomic<uint64_t> walk_sync_found_{0};
    std::atomic<uint64_t> walk_frames_ok_{0};
    std::atomic<uint64_t> walk_adv_frame_{0};   // advanced past a decoded frame
    std::atomic<uint64_t> walk_adv_ref_{0};     // advanced by the preamble ref
    std::atomic<uint64_t> walk_exit_nosync_{0};
    std::atomic<uint64_t> walk_exit_maxframes_{0};
    std::atomic<uint64_t> walk_exit_eow_{0};
    std::atomic<uint64_t> walk_frames_in_window_{0}; // sum, for frames/window

    // Post-TimingSync demodulation quality. Truncation and sync acquisition
    // are identical at 1 and 2 MHz (254 vs 253 truncated, 67.2 vs 66.7%
    // sync), but CRC failures rise 20x (13 -> 264), so the loss is in
    // demodulation quality rather than framing. These separate a carrier
    // problem from a timing problem.
    std::atomic<uint64_t> q_n_{0};
    std::atomic<uint64_t> q_freq_abs_ur_{0};  // |freq_per_sym|, micro-radians/sym
    std::atomic<uint64_t> q_qual_milli_{0};   // DAS correlation quality x1000
    std::atomic<uint64_t> q_evm_acq_pct_{0};  // EVM over acquisition symbols, %
    std::atomic<uint64_t> q_evm_pay_pct_{0};  // EVM over payload symbols, %
    std::atomic<uint64_t> q_evm_n_{0};
    // Split by CRC outcome. Whether a frame's EVM was already bad at the
    // acquisition section, or only went bad across the payload, is what
    // separates a carrier-estimate fault from a tracking fault -- and
    // comparing pass against fail separates either from plain SNR loss,
    // which would raise both buckets together.
    std::atomic<uint64_t> q_pass_n_{0}, q_pass_acq_{0}, q_pass_pay_{0}, q_pass_cfo_ur_{0};
    std::atomic<uint64_t> q_fail_n_{0}, q_fail_acq_{0}, q_fail_pay_{0}, q_fail_cfo_ur_{0};

    // Structural parsing. At 2 MHz, CRC-failed frames are indistinguishable
    // from passed ones in EVM (69.69 vs 69.67%) and residual CFO (0.00205 vs
    // 0.00201), so the corruption is not in the symbols. These look at what
    // the header claimed and whether the byte stream matched it.
    std::atomic<uint64_t> st_pass_plen_{0}, st_fail_plen_{0};
    std::atomic<uint64_t> st_pass_inv_{0},  st_fail_inv_{0};   // BPSK 180-deg ambiguity
    std::atomic<uint64_t> st_pass_short_{0}, st_fail_short_{0};// bytes < header+plen+crc
    std::atomic<uint64_t> st_pass_aggr_{0},  st_fail_aggr_{0}; // FL_AGGR set
    std::atomic<uint64_t> st_fail_modbad_{0};                  // payload mod != tx_mod_

    // Per-frame error TAIL. A frame dies on its worst symbols, not its mean.
    std::atomic<uint64_t> t_pass_n_{0}, t_pass_p95_{0}, t_pass_p99_{0},
                          t_pass_max_{0}, t_pass_weak_{0};
    std::atomic<uint64_t> t_fail_n_{0}, t_fail_p95_{0}, t_fail_p99_{0},
                          t_fail_max_{0}, t_fail_weak_{0};   // ARQ window full

    // Aggregation receive accounting: records seen inside decoded frames vs
    // records the TAP actually accepted. A decoded frame counts as good if
    // any one record lands, so without these a silent per-record loss looks
    // like a radio problem rather than a local write problem.
    std::atomic<uint64_t> rx_records_{0};
    std::atomic<uint64_t> rx_rec_written_{0};
    std::atomic<uint64_t> rx_rec_failed_{0};

    // Which of the three alignment neighbours actually yielded the decode.
    // PreambleSync's peak was historically a sample or two off what
    // TimingSync wanted, so each burst is retried at base, base-1, base+1.
    // That retry is ~1.8 correlations per burst; these counters say whether
    // it still earns its cost.
    std::array<std::atomic<uint64_t>, 3> align_hits_{};
    std::atomic<uint64_t>                align_attempts_{0};

    // Throttles the display-only FFT spectrum -- see cfg_.spectrum_interval_ms.
    std::chrono::steady_clock::time_point last_spectrum_{};

    std::atomic<bool>  running_{false};
    std::thread        tx_thread_;
    std::thread        capture_thread_;
    std::thread        rx_thread_;
    std::thread        stat_thread_;
    std::atomic<uint32_t> tx_seq_{0};

    // Capture -> DSP handoff. Demodulating a burst takes long enough that
    // doing it inline with rxPull() left the receiver deaf ~30% of the time,
    // and a frame straddling one of those gaps is captured only partially --
    // enough energy for the burst detector to fire, but never decodable. A
    // dedicated capture thread keeps reception continuous; the DSP thread
    // drains this queue. Bounded, dropping oldest, so a slow DSP thread
    // costs whole frames rather than unbounded memory.
    static constexpr size_t CAPTURE_QUEUE_MAX = 8;
    std::deque<std::vector<int16_t>> capture_queue_;
    std::mutex                       capture_mu_;
    std::condition_variable          capture_cv_;

    // Diagnostic: if $SDR_FRAME_LOG is set, every decoded frame (successful
    // or CRC-failed) is logged there with its content, for manual RF-link
    // analysis. Off by default (empty path -> no file opened).
    std::ofstream frame_log_;

    // Diagnostic: if $SDR_RAW_LOG is set, every raw demodulated byte (before
    // framing) is dumped there as hex, for manually inspecting what the
    // receiver actually produced regardless of whether a frame was found.
    std::ofstream raw_log_;

    // Diagnostic: if $SDR_IQ_DUMP is set, the raw capture stream is written
    // there (interleaved int16 I/Q, the same format the offline analysis
    // tools read), bounded by $SDR_IQ_DUMP_MB. The radio is opened
    // exclusively, so this is the only way to see what the receiver is
    // actually being fed during a live two-node link.
    std::ofstream iq_dump_;
    size_t        iq_dumped_     {0};
    size_t        iq_dump_limit_ {0};

    static constexpr size_t IQ_SAMPLES = 256 * 1024;

    // Cap on frames decoded from a single detected window. Under continuous
    // traffic one window spans many back-to-back frames; the cap bounds the
    // worst-case work per window so the DSP thread cannot stall the capture
    // queue behind one pathological buffer.
    static constexpr int MAX_FRAMES_PER_WINDOW = 64;

    // NOTE: the old brute-force alignment sweep (retrying the whole DSP
    // chain at every RRC_TAPS offset) has been replaced by PreambleSync,
    // which correlates against the preamble+sync-word to compute the frame
    // start directly. See rxThread().
};

} // namespace sdr
