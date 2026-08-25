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
    void txPusherThread();
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
    // How long a partially-filled TX buffer may wait before being sent.
    // Larger batches amortise the ~10 ms fixed cost of an iio push, but bound
    // it so a quiet link does not sit on queued data. Overridable for sweeps.
    static constexpr int TX_LATENCY_MS   = 8;
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

    // Walk-advance accuracy. Enlarging the burst window measured WORSE (more
    // bursts found, fewer frames each), so the receiver is not window-limited
    // -- the walk may simply be predicting the next preamble's location
    // badly, and preamble_search only looks ~1536 samples from wherever it
    // lands. These record predicted-vs-actual so that can be settled.
    std::atomic<uint64_t> wk_pred_n_{0};        // advances followed by a search
    std::atomic<uint64_t> wk_hit_{0};           // preamble found within the bound
    std::atomic<uint64_t> wk_miss_{0};          // not found -> walk ends
    std::atomic<int64_t>  wk_err_sum_{0};       // signed offset error, samples
    std::atomic<uint64_t> wk_err_abs_sum_{0};
    std::atomic<uint64_t> wk_err_max_{0};
    std::atomic<uint64_t> wk_near_edge_{0};     // found in the last 25% of the bound
    // ── Failed-decode recovery ($SDR_PSYNC_LOG, [WR] lines) ──────────────
    // When a frame fails to decode the walk steps forward by only
    // referenceLength() (1120 samples) and searches 1536 from there -- but a
    // frame is ~18600 samples, so the next search starts and ends INSIDE the
    // frame that just failed, over payload that cannot correlate. Measured:
    // the post-failure class fails 100% of the time (590/590), and 427 of the
    // 565 frames still lost are interior frames abandoned this way.
    //
    // These record whether the advance was simply wrong -- i.e. a decodable
    // preamble sat beyond the search region -- or whether nothing was left.
    // Advances taken past a frame that failed to decode, using the header's
    // own length rather than a blind template step.
    std::atomic<uint64_t> walk_adv_hdr_{0};
    std::atomic<uint64_t> wr_fails_{0};        // failed-decode advances taken
    std::atomic<uint64_t> wr_header_ok_{0};    // ... where the header DID decode,
                                               // so the true frame end was known
    std::atomic<uint64_t> wr_complete_{0};     // ... and the payload was all present
    std::atomic<uint64_t> wr_probe_n_{0};      // recovery probes run
    std::atomic<uint64_t> wr_next_within_{0};  // next preamble inside the search bound
    std::atomic<uint64_t> wr_next_beyond_{0};  // exists, but beyond it -> ABANDONED
    std::atomic<uint64_t> wr_next_none_{0};    // nothing left in the window
    std::atomic<uint64_t> wr_dist_sum_{0};     // distance from the resume point
    std::atomic<uint64_t> wr_dist_max_{0};
    // Where a header-derived advance WOULD have landed, versus where the walk
    // actually resumed.
    std::atomic<uint64_t> wr_hdr_adv_n_{0};
    std::atomic<uint64_t> wr_hdr_adv_gap_{0};  // computable_end - actual_resume

    // ── Capture-batch carry-over ─────────────────────────────────────────
    // rxPull hands the DSP thread a fixed-size buffer, and a burst that
    // starts in its back half runs off the end. The detector then reports a
    // window clipped at the buffer edge, the walk decodes frames until fewer
    // than one correlation reference remains, and the rest of the burst --
    // sitting at the head of the NEXT buffer -- is never joined to it.
    //
    // Measured before the fix, at a 130368-sample burst against a 262144-
    // sample capture buffer: of bursts that lost frames, 79.2% had their
    // window end within 4096 samples of the buffer edge, against 0.5% of
    // bursts that were fully recovered. Frames recovered tracked window
    // shortfall almost exactly -- short by N frames, lose N frames.
    //
    // Fixed by DEFERRING the clipped window instead of demodulating a
    // fragment: its samples are held and prepended to the next batch, so the
    // burst is presented to the detector whole, exactly once. Deferring
    // rather than overlapping is what keeps it exactly once -- an overlap
    // buffer would re-present frames that had already been delivered.
    std::atomic<uint64_t> rx_trunc_windows_{0};  // windows clipped at the buffer edge
    std::atomic<uint64_t> rx_carry_events_{0};   // ... that were deferred
    std::atomic<uint64_t> rx_carry_samples_{0};  // total samples carried
    std::atomic<uint64_t> rx_carry_max_{0};
    // Deferral refused, split by WHICH bound bound: conflating them hides
    // whether the cap or the hold-time is the binding constraint.
    std::atomic<uint64_t> rx_carry_refused_cap_{0};      // tail bigger than the cap
    std::atomic<uint64_t> rx_carry_refused_batches_{0};  // held for too many batches
    std::atomic<uint64_t> rx_carry_stitched_{0}; // deferred windows later processed
    std::atomic<uint64_t> rx_carry_cap_{0};      // $SDR_RX_CARRY_MAX, samples

    // ── PreambleSync correlation-quality trace ($SDR_PSYNC_LOG) ──────────
    // The burst-boundary run showed EVERY window ends on no_sync, so the loss
    // is PreambleSync failing to correlate -- 25% of windows never find their
    // first preamble, and 14% of post-decode advances never find the next.
    // These separate "the preamble is not there / not aligned" from "it is
    // there and MIN_QUALITY rejected it", which call for opposite fixes.
    std::ofstream psync_log_;

    // Attempt classes. The two failure populations behave differently and
    // must never be pooled: one is cold acquisition, the other is walk state.
    enum PsyncClass { PS_FIRST = 0,   // first search in a window (cold)
                      PS_AFTER_OK,    // after a frame decoded (walk advance)
                      PS_AFTER_FAIL,  // after a sync hit that failed to decode
                      PS_NCLASS };
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_n_{};
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_fail_{};
    // Peak quality, in milli-units, summed over attempts. Kept separately for
    // hits and misses: the miss mean is the number that says whether the
    // threshold is the problem.
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_q_hit_mil_{};
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_q_miss_mil_{};
    // Failures whose peak landed just under MIN_QUALITY -- a threshold call,
    // not an absent signal.
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_near_thresh_{};
    // Peak-to-second-peak ratio on hits, milli-units. A ratio near 1 means
    // the winner was not distinctive and the offset may be the wrong lobe.
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_ratio_mil_{};
    std::array<std::atomic<uint64_t>, PS_NCLASS> ps_ratio_n_{};

    // Failure post-mortem (SDR_PSYNC_PROBE=<every-Nth>).
    std::atomic<uint64_t> ps_probe_n_{0};
    std::atomic<uint64_t> ps_probe_found_wide_{0};   // peak appeared at df=0, wider range
    std::atomic<uint64_t> ps_probe_found_cfo_{0};    // peak appeared only at df!=0
    std::atomic<uint64_t> ps_probe_nothing_{0};      // no peak at any hypothesis
    std::atomic<int64_t>  ps_probe_df_sum_{0};       // Hz, over ps_probe_found_cfo_
    std::atomic<uint64_t> ps_probe_q_mil_{0};        // best q found, milli

    std::atomic<uint64_t> wk_bias_applied_{0};  // current advance bias, samples
    std::atomic<uint64_t> wk_bias_sum_{0};      // for the mean actually used
    std::atomic<uint64_t> wk_bias_n_{0};

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
                          t_fail_max_{0}, t_fail_weak_{0};
    // Position of the damage within the frame.
    std::atomic<uint64_t> t_pass_wpos_{0}, t_pass_wfq_{0}, t_pass_wlq_{0};
    std::atomic<uint64_t> t_fail_wpos_{0}, t_fail_wfq_{0}, t_fail_wlq_{0};
    // Either side of the Costas payload tap: does it cause the early-payload
    // transient or inherit it?
    std::atomic<uint64_t> cs_probe_n_{0};
    std::atomic<uint64_t> cs_in_amp_ratio_{0},  cs_out_amp_ratio_{0};
    std::atomic<uint64_t> cs_in_ph_q1_{0},  cs_in_ph_rest_{0};
    std::atomic<uint64_t> cs_out_ph_q1_{0}, cs_out_ph_rest_{0};

    // Byte-level TX/RX capture, for diffing what was sent against what
    // arrived. RS repairs only 10% of 2 MHz failures despite BER ~9e-6, and
    // failed frames show EVM identical to passing ones -- so the corruption
    // may not be random bit errors at all. Records are
    // [u32 seq][u32 len][bytes] over the CRC-covered body, keyed on the seq
    // field at body offset 12.
    std::ofstream tx_dump_;
    std::ofstream rx_fail_dump_;
    // NCO state trace for slip diagnosis (SDR_SLIPTRACE). Kept from the last
    // FixedTimingSync::process() call so a frame that fails CRC can have its
    // timing-loop state dumped for the symbols it occupied.
    std::ofstream slip_trace_;
    FixedTimingSync::Result last_fts_;
    int slip_frames_dumped_{0};
    int slip_pass_dumped_{0};   // control group: frames that PASSED CRC
    int costas_seed_{0};        // 0=none 1=LS freq 2=residual fraction
    float costas_phase_limit_{0.f};      // quadrant-slip guard, radians (0 = off)
    // Per-symbol carrier rotation applied by the Costas loop, recovered as
    // arg(out * conj(in)) so no change to CostasLoop is needed. A QPSK Costas
    // has four-fold phase ambiguity; a mid-frame slip of ~pi/2 re-maps every
    // subsequent symbol's bits while leaving each symbol perfectly on a
    // constellation point -- which is exactly the observed signature.
    std::vector<float> last_carrier_phase_;   // ARQ window full

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
    std::thread             tx_pusher_thread_;
    std::deque<std::vector<int16_t>> tx_sample_q_;
    std::deque<size_t>      tx_sample_frames_;   // frames carried by each buffer
    // Frame sequence numbers carried by each buffer, in transmit order.
    // A "TX burst" is exactly one of these buffers: one txPush, one
    // contiguous run of samples on the air, with a duty gap either side.
    // Recording the seqs is what lets the peer's per-window log be joined
    // back to the burst those frames were actually sent in -- the two nodes
    // share no clock, but they do share the sequence numbering.
    std::deque<std::vector<uint32_t>> tx_sample_seqs_;
    std::mutex              tx_sq_mu_;
    std::condition_variable tx_sq_cv_;
    std::atomic<uint64_t>   tx_sq_stalls_{0};    // worker waited for room
    static constexpr size_t TX_SAMPLE_Q_MAX = 3;
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

    // ── Burst-boundary instrumentation ($SDR_BURST_LOG) ───────────────────
    // Answers one question: does one transmitted burst arrive as one detected
    // RX window, or is the receiver chopping it into several?
    //
    // The transmitter writes a [TXB] line per pushed buffer (burst id, frame
    // count, sample count, the seqs it carried); the receiver writes an [RXW]
    // line per detected window (raw and extended bounds, overlap with the
    // previous window, frames extracted, the seqs decoded). The two logs live
    // on different nodes and are joined offline by seq --
    // scripts/rf/burst_join.py.
    //
    // Written from txPusherThread and rxThread, hence the mutex.
    std::ofstream burst_log_;
    std::mutex    burst_log_mu_;
    std::atomic<uint64_t> tx_burst_id_{0};

    // RX-side summary, so the basic verdict does not require the peer's log.
    std::atomic<uint64_t> bd_batches_{0};        // capture buffers with >=1 window
    std::atomic<uint64_t> bd_windows_{0};
    std::atomic<uint64_t> bd_win_empty_{0};      // window yielded no frame at all
    std::atomic<uint64_t> bd_frames_{0};         // frames extracted from windows
    // Extended windows (win.start .. win_end) that overlap their predecessor:
    // the same samples are demodulated twice, once per window.
    std::atomic<uint64_t> bd_overlap_pairs_{0};
    std::atomic<uint64_t> bd_overlap_samples_{0};
    std::atomic<uint64_t> bd_overlap_max_{0};
    // Raw detector regions (pre-extension) and the gaps between them. A gap
    // just above burst_merge_gap is a burst the merge step declined to rejoin.
    std::atomic<uint64_t> bd_gap_pairs_{0};
    std::atomic<uint64_t> bd_gap_sum_{0};
    std::atomic<uint64_t> bd_gap_min_{~uint64_t(0)};
    std::atomic<uint64_t> bd_gap_near_merge_{0};  // gap < 4x merge_gap
    // Sequence continuity across window boundaries. If a window's first
    // decoded seq is exactly one past the previous window's last decoded seq,
    // those two windows were carrying consecutive frames -- which is what a
    // single TX burst split in two looks like from the receiver alone.
    std::atomic<uint64_t> bd_seq_pairs_{0};      // consecutive windows both decoding
    std::atomic<uint64_t> bd_seq_contig_{0};     // ... and seq continued exactly
    std::atomic<uint64_t> bd_seq_contig_same_batch_{0};
    // Frames-per-window histogram: 0,1,2,3,4,5,6-9,10+
    std::array<std::atomic<uint64_t>, 8> bd_fpw_{};

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
