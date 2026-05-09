/*
 * rssi_meter.cpp  —  Hardware RSSI / signal power accumulator for Zynq-7010
 *
 * Computes I² + Q² (instantaneous power) on every IQ sample in FPGA fabric
 * using DSP48 slices.  Accumulates over a configurable window and exposes
 * the result as AXI4-Lite registers that Linux/libiio can read.
 *
 * This replaces the software getRSSI() polling of the AD9363 iio attribute,
 * giving per-symbol power measurements at full sample rate (20 MSPS) with
 * zero CPU involvement.
 *
 * ── AXI4-Lite registers ───────────────────────────────────────────────
 *   0x10  window_log2  RW  log2 of accumulation window (default 10 = 1024 samples)
 *   0x18  power_accum  RO  accumulated I²+Q² over last window (32-bit, saturating)
 *   0x20  rssi_valid   RO  1 when power_accum holds a fresh measurement
 *   0x28  peak_power   RO  maximum single-sample I²+Q² in last window
 *
 * ── AXI4-Stream ───────────────────────────────────────────────────────
 *   s_axis_iq : IQ samples from cf-ad9361-lpc (32-bit: I[15:0] Q[31:16])
 *   m_axis_iq : pass-through (same data, unmodified) → next IP or DMA
 *
 * ── Resource estimate (Zynq-7010) ────────────────────────────────────
 *   2 DSP48E1 (one for I², one for Q²)
 *   ~150 LUTs, ~200 FFs
 *   Fmax: 250 MHz (well above 200 MHz AD9363 clock)
 */

#include "ap_fixed.h"
#include "ap_int.h"
#include "hls_stream.h"

struct IQSample {
    ap_int<16> i;
    ap_int<16> q;
    ap_uint<1> last;
};

void rssi_meter_top(
    hls::stream<IQSample>& s_axis_iq,
    hls::stream<IQSample>& m_axis_iq,
    volatile ap_uint<5>&   window_log2,   // 0x10: accumulation window = 2^window_log2
    volatile ap_uint<32>&  power_accum,   // 0x18: result register
    volatile ap_uint<1>&   rssi_valid,    // 0x20: fresh result flag
    volatile ap_uint<32>&  peak_power)    // 0x28: per-window peak
{
#pragma HLS INTERFACE axis        port=s_axis_iq
#pragma HLS INTERFACE axis        port=m_axis_iq
#pragma HLS INTERFACE s_axilite   port=window_log2  offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=power_accum  offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=rssi_valid   offset=0x20 bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=peak_power   offset=0x28 bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=return       bundle=ctrl
#pragma HLS PIPELINE II=1

    // Use DSP48 for multiplications
#pragma HLS BIND_OP variable=return op=mul impl=dsp

    static ap_uint<32> accum    = 0;
    static ap_uint<32> peak     = 0;
    static ap_uint<20> count    = 0;
    static ap_uint<5>  win_log2 = 10;   // default 1024 samples

    if (s_axis_iq.empty()) return;

    IQSample in = s_axis_iq.read();

    // I² + Q²  —  each product fits in 32 bits (16×16 = 32)
    ap_uint<32> i2 = (ap_uint<32>)((ap_int<32>)in.i * in.i);
    ap_uint<32> q2 = (ap_uint<32>)((ap_int<32>)in.q * in.q);
    ap_uint<32> pwr = i2 + q2;

    // Saturating accumulate
    ap_uint<32> new_accum = accum + pwr;
    if (new_accum < accum) new_accum = 0xFFFFFFFF;  // saturate
    accum = new_accum;

    if (pwr > peak) peak = pwr;
    count++;

    ap_uint<20> window_size = (ap_uint<20>)(1 << win_log2);
    if (count >= window_size) {
        // Latch results
        power_accum = accum >> win_log2;   // mean power
        peak_power  = peak;
        rssi_valid  = 1;
        // Reset for next window
        accum    = 0;
        peak     = 0;
        count    = 0;
        win_log2 = window_log2;            // pick up any config change
    }

    // Pass IQ through unmodified
    m_axis_iq.write(in);
}
