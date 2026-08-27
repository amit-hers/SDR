/*
 * gain_block.cpp  —  AXI4-Lite controlled digital gain stage for Zynq-7010
 *
 * Multiplies IQ samples by a configurable gain before TX or after RX.
 * Used for:
 *   • Fine digital attenuation without round-tripping through the AD9363
 *     slow-attack AGC (complement to hardware setTxAttenuation).
 *   • TX power ramp-up / ramp-down on burst edges to avoid spectral splatter.
 *   • Safety gate: force gain=0 if samples exceed amplitude_limit.
 *
 * ── AXI4-Lite registers ───────────────────────────────────────────────
 *   0x10  gain_q12    RW  gain as Q0.12 fixed-point (0x1000 = 1.0, max 4095/4096)
 *   0x18  enabled     RW  1 = apply gain, 0 = pass through
 *   0x20  amp_limit   RW  per-sample |I| or |Q| threshold — if exceeded, gain→0
 *   0x28  clip_count  RO  number of samples clipped (saturated to ±32767)
 *   0x30  gate_count  RO  number of times PA safety gate tripped
 *
 * ── AXI4-Stream ───────────────────────────────────────────────────────
 *   s_axis_iq : input IQ (16-bit I + 16-bit Q + last)
 *   m_axis_iq : output IQ (same format, gain applied)
 *
 * ── Resource estimate (Zynq-7010) ────────────────────────────────────
 *   2 DSP48E1, ~120 LUTs, ~160 FFs
 */

#include "ap_fixed.h"
#include "ap_int.h"
#include "hls_stream.h"

struct IQSample {
    ap_int<16> i;
    ap_int<16> q;
    ap_uint<1> last;
};

// Saturate 32-bit result to 16-bit range
static ap_int<16> sat16(ap_int<32> v) {
#pragma HLS INLINE
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (ap_int<16>)v;
}

void gain_block_top(
    hls::stream<IQSample>& s_axis_iq,
    hls::stream<IQSample>& m_axis_iq,
    volatile ap_uint<13>&  gain_q12,     // 0x10: Q0.12 gain (0x1000 = unity)
    volatile ap_uint<1>&   enabled,      // 0x18
    volatile ap_uint<16>&  amp_limit,    // 0x20: PA safety gate threshold
    volatile ap_uint<32>&  clip_count,   // 0x28: RO
    volatile ap_uint<32>&  gate_count)   // 0x30: RO
{
#pragma HLS INTERFACE axis       port=s_axis_iq
#pragma HLS INTERFACE axis       port=m_axis_iq
#pragma HLS INTERFACE s_axilite  port=gain_q12    offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=enabled     offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=amp_limit   offset=0x20 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=clip_count  offset=0x28 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=gate_count  offset=0x30 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=return      bundle=ctrl
#pragma HLS PIPELINE II=1

    static ap_uint<32> clips = 0;
    static ap_uint<32> gates = 0;

    // AXI-Lite control registers are `volatile ap_uint<N>&`, and clang-16
    // (Vitis HLS 2026.1) no longer converts those implicitly to bool:
    //   ERROR: [HLS 207-4589] no viable conversion from 'volatile ap_uint<1>' to 'bool'
    // Sampling each register once into a local is both the fix and the
    // better hardware: a volatile reference re-reads the register on every
    // access, so a control value could otherwise change midway through the
    // computation it is steering.
    // Copy out of the volatile reference FIRST, then test the copy. Comparing
    // the volatile operand directly still fails:
    //   ERROR: [HLS 207-4572] invalid operands to binary expression
    //                         ('volatile ap_uint<1>' and 'int')
    // ap_int_base has a volatile-aware copy constructor but no volatile
    // comparison operators, so the copy is what makes this legal.
    const ap_uint<1>  en_raw  = enabled;
    const ap_uint<13> gain_l  = gain_q12;
    const ap_uint<16> limit_l = amp_limit;
    const bool        en_l    = (en_raw != 0);

    if (s_axis_iq.empty()) return;

    IQSample in  = s_axis_iq.read();
    IQSample out = in;   // default: pass through

    if (en_l) {
        ap_uint<16> limit = limit_l;

        // PA safety gate: if either I or Q exceeds limit, zero the sample
        ap_uint<16> abs_i = (in.i < 0) ? (ap_uint<16>)(-in.i) : (ap_uint<16>)in.i;
        ap_uint<16> abs_q = (in.q < 0) ? (ap_uint<16>)(-in.q) : (ap_uint<16>)in.q;

        if (limit > 0 && (abs_i > limit || abs_q > limit)) {
            out.i = 0;
            out.q = 0;
            gates++;
            gate_count = gates;
        } else {
            // Apply Q0.12 gain: result = (sample * gain) >> 12
            ap_int<32> gi = ((ap_int<32>)in.i * (ap_int<32>)gain_q12) >> 12;
            ap_int<32> gq = ((ap_int<32>)in.q * (ap_int<32>)gain_q12) >> 12;

            // Saturate and count clips
            bool clip = (gi > 32767 || gi < -32768 || gq > 32767 || gq < -32768);
            if (clip) { clips++; clip_count = clips; }

            out.i = sat16(gi);
            out.q = sat16(gq);
        }
    }

    m_axis_iq.write(out);
}
