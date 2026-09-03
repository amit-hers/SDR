#include "qpsk_common.h"

/* ── TX path: QPSK modulator (ARM → FPGA → AD9363 DAC) ──────────────
 * Takes byte stream, outputs RRC-pulse-shaped IQ at SPS=4.
 * RRC interpolation uses a proper polyphase FIR (same coefficients as
 * the RX matched filter, applied to each of 4 phases in sequence).
 */
void qpsk_mod_top(hls::stream<BitByte>&  s_axis_bits,
                  hls::stream<IQSample>& m_axis_iq,
                  volatile ap_uint<1>&   enabled,       // 0x10: gate TX output
                  volatile ap_uint<2>&   bpsk_mode,     // 0x18: 0=QPSK, 1=BPSK
                  volatile ap_uint<1>&   diff_mode)     // 0x20: 1=differential
{
#pragma HLS INTERFACE axis       port=s_axis_bits
#pragma HLS INTERFACE axis       port=m_axis_iq
#pragma HLS INTERFACE s_axilite  port=enabled    offset=0x10 bundle=ctrl
/* Offsets are on HLS's 8-byte grid (each scalar gets data + a reserved word),
 * so they are actually honoured. bpsk_mode was requested at 0x14 and silently
 * placed at 0x18; asking for 0x14 again while adding a second register pushed
 * it to 0x24 and broke every script writing 0x43C10018. Request what the tool
 * will grant, and check the generated *_hw.h after any interface change. */
#pragma HLS INTERFACE s_axilite  port=bpsk_mode  offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=diff_mode  offset=0x20 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=return     bundle=ctrl
/* PIPELINE II=16, not II=1.
 *
 * II=1 forced every loop below to unroll, replicating the pulse-shaping FIR
 * 4 symbols x 4 phases: 4*4*13*2 = 416 multipliers, 224 mapped to DSP48s.
 * That is 101% of a 7z020 for the modulator alone, and the integrated design
 * failed placement needing 322 DSPs against 220 available.
 *
 * The throughput was never needed. One call takes a byte and emits SPS*4 = 16
 * samples, and the radio consumes one sample per clock, so 16 cycles per byte
 * is exactly right. II=16 lets HLS share one FIR across those cycles.
 *
 * Rolling the loops by hand (dropping this pragma and pipelining the phase
 * loop instead) also cut the DSPs, and was WRONG: C simulation was bit-exact
 * either way, but RTL co-simulation diverged from the second byte onward --
 * correlation 0.551 at lag -5 against 1.000 at lag 0 -- because the static
 * delay line stopped carrying correctly between transactions. csim cannot see
 * that class of fault; only cosim can. Relaxing II keeps the structure that
 * co-simulates clean and lets the scheduler do the sharing.
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
    const ap_uint<1> diff_raw = diff_mode;
    const bool       tx_en    = (tx_raw   != 0);
    const bool       bpsk_en  = (bpsk_raw != 0);
    const bool       diff_en  = (diff_raw != 0);

    /* Differential encoding, off by default.
     *
     * A QPSK slicer is four sign tests, so the receiver recovers the
     * constellation only up to a 90-degree rotation and there is nothing in a
     * continuous stream to resolve which of the four it landed in. Every
     * payload measured over the air in this project was recovered by
     * brute-forcing all four rotations OFFLINE; a live receiver cannot do that.
     *
     * Encoding the DIFFERENCE between consecutive symbols removes the ambiguity
     * outright: a constant rotation r adds r to every transmitted symbol and
     * cancels in the difference. One register and a modulo-4 add.
     *
     * The alternative -- a preamble correlator -- costs far more: 32 symbols
     * against four rotations, inside a core whose modem-clock WNS is already
     * only +0.725 ns at 35 MHz.
     *
     * The cost is the standard differential penalty: about 0.5-1 dB, and one
     * symbol error becomes two because each symbol is decoded against its
     * predecessor. Measured link SNR is ~18 dB, so that is affordable.
     *
     * OFF by default because it changes the wire format. Both ends must agree,
     * and the csim vectors (mod_ref.iq/.bits, generated by the host modulator)
     * are absolute, so they would stop matching if this defaulted on. */
    static ap_uint<2> phase = 0;   // differential phase accumulator

    // When no input is ready, output zero (keeps DMA happy)
    if (s_axis_bits.empty()) {
        goto rrc_out;
    }

    {
        BitByte in = s_axis_bits.read();

        /* Unpack byte → 4 QPSK symbols (2 bits each, MSB first) */
        for (int sym = 0; sym < 4; sym++) {
            ap_uint<2> bits = (in.data >> (6 - sym*2)) & 0x3;
            if (diff_en) {
                /* Accumulate in PHASE, not in the symbol index.
                 *
                 * qpsk_decision numbers symbols (bq<<1)|bi, which walks the
                 * circle as 0, 1, 3, 2 -- Gray order. A 90-degree rotation is
                 * therefore NOT "+1 in the symbol index", so differencing the
                 * index does not cancel it. Measured: differencing the index
                 * recovers only the 0-degree case, exactly like absolute QPSK.
                 *
                 * Working in phase and converting with bin->Gray fixes it: a
                 * rotation of k quadrants adds k to every phase and cancels in
                 * the difference. */
                phase    = (ap_uint<2>)(phase + bits);
                bits     = (ap_uint<2>)(phase ^ (phase >> 1));   // bin -> Gray
            }
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
            for (int k = 0; k < RRC_SPS_HW; k++) {
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
