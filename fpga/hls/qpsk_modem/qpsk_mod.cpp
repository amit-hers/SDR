#include "qpsk_common.h"

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
/* No function-level PIPELINE II=1.
 *
 * That directive forces every loop below it to unroll completely, which
 * replicated the pulse-shaping FIR 4 symbols x 4 phases over: 4*4*13*2 = 416
 * multipliers, 224 of them mapped to DSP48s. On a 7z020 that is 101% of the
 * device for the modulator alone, and the integrated design failed placement
 * with "requires 322 DSP48E1 cells but only 220 are available".
 *
 * The throughput it bought was never needed. One call consumes a byte and
 * emits SPS*4 = 16 samples, and the radio needs one sample per l_clk, so 16
 * cycles per byte is exactly right. Rolling the symbol and phase loops lets
 * all 16 outputs share a single 13-tap engine -- 26 multipliers rather than
 * 416 -- at the same sample rate. The arithmetic is untouched; only the
 * schedule changes, which is why the C simulation is bit-identical.
 */

    /* RRC TX delay line, at the SYMBOL rate.
     *
     * One entry per symbol of pulse history, not one per tap: sub-filter k
     * reads taps k, k+SPS, k+2*SPS, ..., so it only ever touches
     * TAPS_PER_PHASE = ceil(49/4) = 13 symbols. This was NTAPS (49) deep and
     * shifted all 49 entries every symbol; entries 13..48 were written, never
     * read, and cost 36 x 2 x 16 = 1152 flops plus a shift network almost four
     * times wider than needed. Output is bit-identical at 13.
     */
    static fixp_t tx_delay_i[TAPS_PER_PHASE] = {};
    static fixp_t tx_delay_q[TAPS_PER_PHASE] = {};
#pragma HLS ARRAY_PARTITION variable=tx_delay_i complete
#pragma HLS ARRAY_PARTITION variable=tx_delay_q complete

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
        goto rrc_out;
    }

    {
        BitByte in = s_axis_bits.read();

        /* Unpack byte → 4 QPSK symbols (2 bits each, MSB first) */
        for (int sym = 0; sym < 4; sym++) {
            ap_uint<2> bits = (in.data >> (6 - sym*2)) & 0x3;
            fixp_t i_sym, q_sym;

            if (bpsk_en) {
                // BPSK: I = ±1, Q = 0
                i_sym = bits[1] ? fixp_t(-1.0f) : fixp_t(1.0f);
                q_sym = fixp_t(0.0f);
            } else {
                /* QPSK, in the host's constellation. The peer is liquid-dsp's
                 * LIQUID_MODEM_QPSK, which maps the symbol as
                 *     I = (sym & 1) ? -1/sqrt(2) : +1/sqrt(2)
                 *     Q = (sym & 2) ? -1/sqrt(2) : +1/sqrt(2)
                 * and Modem::modulate packs the FIRST bit of the pair into the
                 * MSB. So the first bit steers Q and the second steers I.
                 *
                 * This had them the other way round. A swapped I/Q is a
                 * reflection about the diagonal, not a rotation, so no phase
                 * the receiver can lock to undoes it: correlation against the
                 * host reference measured 0.0444 -- noise. It is also why the
                 * failure looked like a filter bug; the polyphase indexing
                 * below was correct all along.
                 */
                i_sym = (bits[0] == 0) ? fixp_t( 0.707f) : fixp_t(-0.707f);
                q_sym = (bits[1] == 0) ? fixp_t( 0.707f) : fixp_t(-0.707f);
            }

            // Shift the symbol-rate history and insert the new symbol at [0],
            // so tx_delay[t] is the symbol t ago -- the ordering the tap index
            // k + t*SPS below assumes.
            for (int k = TAPS_PER_PHASE-1; k > 0; k--) {
#pragma HLS UNROLL
                tx_delay_i[k] = tx_delay_i[k-1];
                tx_delay_q[k] = tx_delay_q[k-1];
            }
            tx_delay_i[0] = i_sym;
            tx_delay_q[0] = q_sym;

            /* Output SPS samples per symbol through a real polyphase FIR.
             *
             * Two defects here, and they compounded:
             *
             *   The accumulation did not depend on k. All four "phases"
             *   summed the same taps over the same delay line, so the core
             *   emitted four identical samples per symbol -- a zero-order
             *   hold, not pulse shaping. Sub-filter k must use every SPS'th
             *   tap starting at k.
             *
             *   The output scaling was `(ap_int<16>)(x * fixp_t(32767))`.
             *   fixp_t is ap_fixed<16,1> with range [-1,1), so 32767 is not
             *   representable; the product stayed inside [-1,1) and the cast
             *   to ap_int<16> truncated it to 0. The modulator emitted zeros.
             *   Q1.15 and int16 share a bit pattern, so the conversion is a
             *   reinterpretation -- the mirror of the receive-side fix.
             */
            /* One output sample per iteration, II=1: this is the loop that
             * sets the sample rate, and the only one that needs to pipeline.
             * The tap loop inside stays unrolled so each sample is a single
             * 13-tap multiply-accumulate. */
            for (int k = 0; k < RRC_SPS_HW; k++) {
#pragma HLS PIPELINE II=1
                acc_t acc_i = 0, acc_q = 0;
                for (int t = 0; t < TAPS_PER_PHASE; t++) {
#pragma HLS UNROLL
                    const int ti = t * RRC_SPS_HW + k;
                    if (ti < NTAPS) {
                        acc_i += (acc_t)(tx_delay_i[t] * RRC_H[ti]);
                        acc_q += (acc_t)(tx_delay_q[t] * RRC_H[ti]);
                    }
                }
                /* Saturate into Q1.15, then reinterpret those bits as int16. */
                const acc_t LIM = acc_t(0.999f);
                if (acc_i >  LIM) acc_i =  LIM;
                if (acc_i < -LIM) acc_i = -LIM;
                if (acc_q >  LIM) acc_q =  LIM;
                if (acc_q < -LIM) acc_q = -LIM;
                fixp_t si_ = (fixp_t)acc_i, sq_ = (fixp_t)acc_q;
                IQSample out;
                out.data = 0;
                if (tx_en) {
                    out.data.range(15,  0) = si_.range(15, 0);
                    out.data.range(31, 16) = sq_.range(15, 0);
                }
                axis_mark(out, sym == 3 && k == RRC_SPS_HW - 1 && in.last != 0);
                m_axis_iq.write(out);
            }
        }
        return;
    }

rrc_out:
    // Flush: output zero sample
    IQSample z; z.data = 0;
    axis_mark(z, false);
    m_axis_iq.write(z);
}
