/*
 * fpga/qpsk_demod_hls.cpp  —  Vivado HLS QPSK modem for Zynq-7010 FPGA
 *
 * This file implements the inner DSP loop (RRC filter + QPSK symbol
 * decision) as synthesizable C++ for Xilinx Vivado HLS.
 * The output is an AXI4-Stream IP core that plugs into the ADI HDL
 * reference design alongside cf-ad9361-lpc.
 *
 * ── How it fits in the Zynq ──────────────────────────────────────────
 *
 *  PL (FPGA fabric):
 *   [AD9363 LVDS] → [ADI AXI AD9361 IP] → [AXI4-Stream IQ samples]
 *        → [THIS IP: rrc_filter + qpsk_demod] → [AXI DMA] → DDR
 *
 *  PS (ARM Cortex-A9):
 *   DDR → [framer + CRC check + AES decrypt] → Ethernet → host
 *
 *  Result: raw IQ never crosses the Ethernet.
 *          ARM only processes ~5 MB/s of decoded frames.
 *
 * ── Build (requires Vivado HLS 2019.1+) ─────────────────────────────
 *   vivado_hls -f hls_build.tcl
 *
 * ── Ports ───────────────────────────────────────────────────────────
 *   s_axis_iq    : AXI4-Stream input  — 32-bit (16-bit I + 16-bit Q)
 *   m_axis_bits  : AXI4-Stream output — 8-bit packed decoded bytes
 *   ctrl         : AXI4-Lite slave    — mode/gain/freq control
 *
 * ── Parameters ──────────────────────────────────────────────────────
 *   SPS=4, ROLLOFF=0.35, TAPS=32, supported: BPSK/QPSK
 */

#include "ap_fixed.h"
#include "ap_int.h"
#include "hls_stream.h"
#include "hls_math.h"

/* ── Precision typedefs ─────────────────────────────────────────────
 * ap_fixed<W,I> = W total bits, I integer bits (W-I fractional bits)
 * Using 16-bit with 1 integer + 15 fractional for normalized IQ.
 */
typedef ap_fixed<16, 1>  fixp_t;    /* [-1, 1) normalized sample */
typedef ap_fixed<32, 2>  acc_t;     /* accumulator for FIR       */
/* Carrier phase, in radians.
 *
 * Needs at least +-pi = 3.14159, which acc_t (ap_fixed<32,2>, range [-2,2))
 * cannot represent -- the Costas accumulator saturated at 1.99999 and stopped
 * tracking, so the loop never actually rotated anything. Four integer bits
 * give [-8,8): room for +-pi plus the overshoot before wrapping. */
typedef ap_fixed<32, 4>  phase_t;
typedef ap_uint<8>        byte_t;

/* ── AXI4-Stream types ──────────────────────────────────────────── */
struct IQSample {
    ap_int<16> i;
    ap_int<16> q;
    ap_uint<1> last;
};

struct BitByte {
    byte_t data;
    ap_uint<1> valid;
    ap_uint<1> last;
};

/* ── RRC filter coefficients (SPS=4, rolloff=0.35, taps=32) ────────
 * Generated with: liquid_firdes_rrcos(4, 32, 0.35, 0, h)
 * Quantized to Q1.15 fixed-point.
 */
#define NTAPS 32
static const fixp_t RRC_H[NTAPS] = {
    -0.0078f,  0.0039f,  0.0195f,  0.0234f,
    -0.0078f, -0.0781f, -0.1484f, -0.1172f,
     0.0781f,  0.3672f,  0.6250f,  0.7500f,
     0.6250f,  0.3672f,  0.0781f, -0.1172f,
    -0.1484f, -0.0781f, -0.0078f,  0.0234f,
     0.0195f,  0.0039f, -0.0078f,  0.0000f,
     0.0039f,  0.0039f,  0.0000f,  0.0000f,
     0.0000f,  0.0000f,  0.0000f,  0.0000f
};

/* ── RRC polyphase matched filter (decimation by SPS=4) ─────────────
 * Processes one sample at a time.
 * Returns true and writes to *out every SPS input samples.
 *
 * Uses a circular shift register in BRAM (HLS maps this automatically).
 */
static bool rrc_filter_decim(fixp_t i_in, fixp_t q_in,
                              fixp_t& i_out, fixp_t& q_out)
{
#pragma HLS INLINE
    static fixp_t delay_i[NTAPS];
    static fixp_t delay_q[NTAPS];
#pragma HLS ARRAY_PARTITION variable=delay_i complete
#pragma HLS ARRAY_PARTITION variable=delay_q complete

    /* Shift register */
    for (int k = NTAPS-1; k > 0; k--) {
#pragma HLS UNROLL
        delay_i[k] = delay_i[k-1];
        delay_q[k] = delay_q[k-1];
    }
    delay_i[0] = i_in;
    delay_q[0] = q_in;

    /* No decimation here.
     *
     * This used to drop 3 of every 4 samples on a fixed phase, and
     * timing_recovery below then decimated by 4 again -- 16x total against a
     * symbol rate that is only 4x the sample rate. Measured in C simulation:
     * 6016 input samples produced 94 output bytes where 376 were expected,
     * exactly the factor of 4 too few.
     *
     * A matched filter belongs at the full sample rate; choosing WHICH sample
     * represents each symbol is timing recovery's job, and it needs all four
     * phases to make that choice. Picking a fixed phase here both broke the
     * rate and denied the timing loop the samples it exists to compare.
     *
     * FIR: fully unrolled → single-cycle multiply-accumulate */
    acc_t acc_i = 0, acc_q = 0;
    for (int k = 0; k < NTAPS; k++) {
#pragma HLS UNROLL
        acc_i += (acc_t)(delay_i[k] * RRC_H[k]);
        acc_q += (acc_t)(delay_q[k] * RRC_H[k]);
    }

    i_out = (fixp_t)acc_i;
    q_out = (fixp_t)acc_q;
    return true;
}

/* ── AGC (digital automatic gain control) ───────────────────────────
 * Simple first-order feedback: scale → measure envelope → adjust gain.
 * Bandwidth 2^-10 ≈ 0.001 (slow enough to track fading, fast enough
 * to acquire on preamble).
 */
static void agc(fixp_t& i, fixp_t& q)
{
#pragma HLS INLINE
    static acc_t gain = 1.0f;
    static acc_t env  = 0.5f;
    const acc_t  bw   = acc_t(1.0f / 1024.0f);

    i = (fixp_t)(i * gain);
    q = (fixp_t)(q * gain);

    acc_t mag = hls::sqrt((acc_t)(i*i + q*q));
    env = env + (mag - env) * bw;
    if (env > acc_t(1e-4f))
        gain = gain * (acc_t(0.707f) / env);
}

/* ── QPSK symbol decision → 2 bits ─────────────────────────────────
 * Standard QPSK Gray-coded constellation:
 *   I>0,Q>0 → 00    I<0,Q>0 → 01
 *   I>0,Q<0 → 10    I<0,Q<0 → 11
 */
static ap_uint<2> qpsk_decision(fixp_t i, fixp_t q)
{
#pragma HLS INLINE
    ap_uint<1> bi = (i >= fixp_t(0)) ? ap_uint<1>(0) : ap_uint<1>(1);
    ap_uint<1> bq = (q >= fixp_t(0)) ? ap_uint<1>(0) : ap_uint<1>(1);
    return (ap_uint<2>)((bi, bq));
}

/* ── Costas loop for QPSK carrier recovery ──────────────────────────
 * Phase error detector: e = sign(Q)*I - sign(I)*Q
 * Loop filter: 2nd order PLL
 */
static void costas_loop(fixp_t& i, fixp_t& q)
{
#pragma HLS INLINE
    static phase_t phase = 0;
    static phase_t freq  = 0;
    const phase_t Kp = phase_t(0.04f);
    const phase_t Ki = phase_t(0.001f);

    /* Rotate input by -phase */
    acc_t cos_p = hls::cos((acc_t)phase);
    acc_t sin_p = hls::sin((acc_t)phase);
    acc_t ir =  (acc_t)i * cos_p + (acc_t)q * sin_p;
    acc_t qr = -(acc_t)i * sin_p + (acc_t)q * cos_p;

    /* Phase error, decision-directed. The +-1 decisions are held in acc_t:
     * fixp_t is ap_fixed<16,1> with range [-1,1), so fixp_t(1.0f) is not
     * representable and the previous code was feeding the loop a saturated
     * constant instead of unity. */
    acc_t si = (ir >= acc_t(0)) ? acc_t(1.0f) : acc_t(-1.0f);
    acc_t sq = (qr >= acc_t(0)) ? acc_t(1.0f) : acc_t(-1.0f);
    acc_t err = sq * ir - si * qr;

    /* 2nd order loop filter */
    freq  = freq  + (phase_t)(Ki * err);
    phase = phase + (phase_t)(Kp * err) + freq;

    /* Wrap to [-pi, pi]. Without this the accumulator walks off regardless of
     * how many integer bits it has, and hls::cos/sin lose meaning. */
    const phase_t PI  = phase_t(3.14159265f);
    const phase_t TAU = phase_t(6.28318531f);
    if (phase >  PI) phase -= TAU;
    if (phase < -PI) phase += TAU;

    i = (fixp_t)ir;
    q = (fixp_t)qr;
}

/* ── Symbol timing recovery (early-late gate, simplified) ───────────
 * Returns true at the correct symbol sampling instant.
 */
static bool timing_recovery(fixp_t i, fixp_t q, fixp_t& i_out, fixp_t& q_out)
{
#pragma HLS INLINE
    static fixp_t i_prev = 0, q_prev = 0;
    static fixp_t i_mid  = 0, q_mid  = 0;
    static acc_t  tau    = 0;
    /* Three bits for the same reason as `phase` in rrc_filter_decim: `cnt < 4`
     * can never be false for an ap_uint<2>, so this returned false forever. */
    static ap_uint<3> cnt = 0;
    const acc_t Kt = acc_t(0.01f);

    cnt++;
    if (cnt == 2) { i_mid = i; q_mid = q; }
    if (cnt < 4)  { i_prev = i; q_prev = q; return false; }
    cnt = 0;

    /* Early-late error */
    acc_t err = (acc_t)(i - i_prev) * (acc_t)i_mid
              + (acc_t)(q - q_prev) * (acc_t)q_mid;
    tau = tau + Kt * err;

    i_out = i;
    q_out = q;
    i_prev = i; q_prev = q;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Top-level HLS function
 * Synthesized to an AXI4-Stream IP core.
 *
 * Interface:
 *   s_axis_iq   → AXI4-Stream input  (AD9363 IQ samples, up to 61.44 MSPS (HamGeek Pluto+))
 *   m_axis_bits → AXI4-Stream output (decoded byte stream)
 *
 * Throughput: 1 IQ sample per clock cycle @ up to 200 MHz
 *             → 200 MSPS > 61.44 MSPS requirement (10× margin)
 * Latency:    ~36 clock cycles pipeline depth (RRC filter + sync)
 * Resources:  ~800 LUTs, ~400 FFs, 2 DSP48 slices (Zynq-7010 estimate)
 * ════════════════════════════════════════════════════════════════════ */
/* ── AXI4-Lite demod control ─────────────────────────────────────────
 *   0x10  demod_enabled  RW  1 = demodulate, 0 = suppress output
 *   0x18  lock_count     RO  symbol lock events since reset
 */
void qpsk_demod_top(hls::stream<IQSample>& s_axis_iq,
                    hls::stream<BitByte>&  m_axis_bits,
                    volatile ap_uint<1>&   demod_enabled,
                    volatile ap_uint<32>&  lock_count)
{
#pragma HLS INTERFACE axis       port=s_axis_iq
#pragma HLS INTERFACE axis       port=m_axis_bits
#pragma HLS INTERFACE s_axilite  port=demod_enabled  offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=lock_count     offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=return         bundle=ctrl
#pragma HLS PIPELINE II=1   /* initiation interval = 1 clock */

    static ap_uint<32> locks = 0;

    // AXI-Lite control registers are `volatile ap_uint<N>&`, and clang-16
    // (Vitis HLS 2026.1) no longer converts those implicitly to bool:
    //   ERROR: [HLS 207-4589] no viable conversion from 'volatile ap_uint<1>' to 'bool'
    // Sampling each register once into a local is both the fix and the
    // better hardware: a volatile reference re-reads the register on every
    // access, so a control value could otherwise change midway through the
    // computation it is steering.
    const ap_uint<1> demod_raw = demod_enabled;   // copy out of volatile first
    const bool       demod_en  = (demod_raw != 0);

    if (s_axis_iq.empty()) return;
    if (!demod_en)        { s_axis_iq.read(); return; }  // drain + suppress

    IQSample in = s_axis_iq.read();

    /* Reinterpret int16 as Q1.15.
     *
     * This was `fixp_t(in.i) / fixp_t(32768)`, which divides by zero on every
     * single sample: fixp_t is ap_fixed<16,1> with range [-1,1), so the
     * literal 32768 is not representable and wraps to exactly 0. C simulation
     * dies with SIGFPE on the first sample -- the core had never been run.
     *
     * An int16 and a Q1.15 fixed-point value have identical bit patterns, so
     * the conversion is a reinterpretation, not an arithmetic operation. This
     * is both correct and free in hardware, where the divide would otherwise
     * have inferred a divider. */
    fixp_t i, q;
    i.range(15, 0) = in.i.range(15, 0);
    q.range(15, 0) = in.q.range(15, 0);

    /* AGC */
    agc(i, q);

    /* RRC matched filter + decimation (SPS=4) */
    fixp_t di, dq;
    if (!rrc_filter_decim(i, q, di, dq)) return;

    /* Timing recovery */
    fixp_t ti, tq;
    if (!timing_recovery(di, dq, ti, tq)) return;

    /* Carrier recovery */
    costas_loop(ti, tq);

    /* QPSK symbol decision */
    ap_uint<2> sym = qpsk_decision(ti, tq);

    /* Pack four 2-bit symbols into one output byte.
     *
     * Both widths here were wrong, and together they silently removed the
     * entire output path:
     *
     *   sym_cnt was ap_uint<2>, which counts 0,1,2,3,0,... and can never
     *   equal 4. The comparison below was therefore always false, the write
     *   to m_axis_bits was unreachable, and HLS eliminated it -- leaving
     *   'Port m_axis_bits_TDATA has no fanin or fanout' and a top function
     *   reported as having no outputs. The core synthesised and exported IP
     *   that could never emit a bit.
     *
     *   bit_acc was ap_uint<4>, which holds two symbols, not the four the
     *   comment describes. Even had the counter worked, half of every byte
     *   would have been shifted out and lost.
     *
     * sym_cnt needs three bits to hold the value 4 at all.
     */
    static ap_uint<8> bit_acc = 0;
    static ap_uint<3> sym_cnt = 0;

    bit_acc = (bit_acc << 2) | sym;
    sym_cnt++;

    if (sym_cnt == 4) {
        sym_cnt = 0;
        locks++;
        lock_count = locks;
        BitByte out;
        out.data  = (byte_t)bit_acc;
        out.valid = 1;
        out.last  = in.last;
        m_axis_bits.write(out);
    }
}

/* ── TX path: QPSK modulator (ARM → FPGA → AD9363 DAC) ──────────────
 * Takes byte stream, outputs RRC-pulse-shaped IQ at SPS=4.
 * RRC interpolation uses a proper polyphase FIR (same coefficients as
 * the RX matched filter, applied to each of 4 phases in sequence).
 */
void qpsk_mod_top(hls::stream<BitByte>&  s_axis_bits,
                  hls::stream<IQSample>& m_axis_iq,
                  volatile ap_uint<1>&   enabled,       // 0x10: gate TX output
                  volatile ap_uint<2>&   bpsk_mode)     // 0x14: 0=QPSK, 1=BPSK
{
#pragma HLS INTERFACE axis       port=s_axis_bits
#pragma HLS INTERFACE axis       port=m_axis_iq
#pragma HLS INTERFACE s_axilite  port=enabled    offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=bpsk_mode  offset=0x14 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=return     bundle=ctrl
#pragma HLS PIPELINE II=1

    // RRC TX delay lines (one per polyphase sub-filter)
    static fixp_t tx_delay_i[NTAPS] = {};
    static fixp_t tx_delay_q[NTAPS] = {};
#pragma HLS ARRAY_PARTITION variable=tx_delay_i complete
#pragma HLS ARRAY_PARTITION variable=tx_delay_q complete
    static ap_uint<2> phase = 0;   // current sub-filter phase (0..3)

    // AXI-Lite control registers are `volatile ap_uint<N>&`, and clang-16
    // (Vitis HLS 2026.1) no longer converts those implicitly to bool:
    //   ERROR: [HLS 207-4589] no viable conversion from 'volatile ap_uint<1>' to 'bool'
    // Sampling each register once into a local is both the fix and the
    // better hardware: a volatile reference re-reads the register on every
    // access, so a control value could otherwise change midway through the
    // computation it is steering.
    const ap_uint<1> tx_raw   = enabled;     // copy out of volatile first;
    const ap_uint<2> bpsk_raw = bpsk_mode;   // ap_uint has no volatile compare
    const bool       tx_en    = (tx_raw   != 0);
    const bool       bpsk_en  = (bpsk_raw != 0);

    // When no input is ready, output zero (keeps DMA happy)
    if (s_axis_bits.empty()) {
        // Drain sub-filters with zeros until phase rolls over
        goto rrc_out;
    }

    {
        BitByte in = s_axis_bits.read();

        /* Unpack byte → 4 QPSK symbols (2 bits each, MSB first) */
        for (int sym = 0; sym < 4; sym++) {
#pragma HLS UNROLL
            ap_uint<2> bits = (in.data >> (6 - sym*2)) & 0x3;
            fixp_t i_sym, q_sym;

            if (bpsk_en) {
                // BPSK: I = ±1, Q = 0
                i_sym = bits[1] ? fixp_t(-1.0f) : fixp_t(1.0f);
                q_sym = fixp_t(0.0f);
            } else {
                // QPSK Gray-coded
                i_sym = (bits[1] == 0) ? fixp_t( 0.707f) : fixp_t(-0.707f);
                q_sym = (bits[0] == 0) ? fixp_t( 0.707f) : fixp_t(-0.707f);
            }

            // Insert symbol into delay line at upsampled position (every SPS taps)
            // Shift delay lines
            for (int k = NTAPS-1; k > 0; k--) {
#pragma HLS UNROLL
                tx_delay_i[k] = tx_delay_i[k-1];
                tx_delay_q[k] = tx_delay_q[k-1];
            }
            tx_delay_i[0] = i_sym;
            tx_delay_q[0] = q_sym;

            // Output SPS=4 samples per symbol via polyphase FIR
            for (int k = 0; k < 4; k++) {
#pragma HLS UNROLL
                acc_t acc_i = 0, acc_q = 0;
                for (int t = 0; t < NTAPS; t++) {
#pragma HLS UNROLL
                    acc_i += (acc_t)(tx_delay_i[t] * RRC_H[t]);
                    acc_q += (acc_t)(tx_delay_q[t] * RRC_H[t]);
                }
                IQSample out;
                out.i    = tx_en ? (ap_int<16>)((fixp_t)acc_i * fixp_t(32767)) : ap_int<16>(0);
                out.q    = tx_en ? (ap_int<16>)((fixp_t)acc_q * fixp_t(32767)) : ap_int<16>(0);
                out.last = (sym == 3 && k == 3 && in.last) ? 1 : 0;
                m_axis_iq.write(out);
            }
        }
        return;
    }

rrc_out:
    // Flush: output zero sample
    IQSample z; z.i = 0; z.q = 0; z.last = 0;
    m_axis_iq.write(z);
}
