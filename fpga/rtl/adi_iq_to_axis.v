// adi_iq_to_axis -- ADI parallel IQ  ->  AXI4-Stream (32-bit, I in [15:0]).
//
// The ADI axi_ad9361 core does not speak AXI4-Stream. It presents parallel
// adc_data_i0/adc_data_q0 (16 bit each) qualified by adc_valid_i0, in the
// l_clk domain, and it has NO backpressure: the converter produces a sample
// whether or not anything downstream is ready. That asymmetry is the whole
// reason this adapter exists, and it is why it carries an overflow flag --
// a stream sink can stall, an ADC cannot.
//
// Sample loss is reported rather than hidden. `overflow` is sticky until
// cleared by reset, because a demodulator that silently skipped samples looks
// exactly like one with a timing bug, and telling those apart after the fact
// is expensive.
//
// No TLAST here: there is no packet boundary in a continuous receive stream.
// Framing belongs on the byte stream after demodulation, where a boundary
// actually means something -- see axis_packetizer.
module adi_iq_to_axis #(
    // Left-shift applied to the converter samples on their way to the modem.
    //
    // The AD9363 delivers 12-bit samples sign-extended into a 16-bit word, so a
    // full-scale input reaches only about 0.06 of the demodulator's Q1.15
    // range, against the ~0.25 its vectors were tuned at. The demod's AGC is
    // meant to make that up, but its gain is clamped at 4 and needs about 16,
    // so it runs ~4x low. Both loop discriminators multiply two samples, so
    // their error scales as amplitude SQUARED and the loops run ~16x slower
    // than their gains assume -- slow acquisition, and intermittent lock over a
    // long capture. Measured in csim: best_run falls from 253 to 169 when
    // clean.iq is scaled down to hardware levels (vectors lowamp4/lowamp8).
    //
    // Fixing it here rather than by opening the AGC clamp keeps signal level
    // and loop dynamics separate: raising the clamp repaired low amplitude but
    // broke the stress and shift50 vectors. Shifting by 3 puts a +/-2048 input
    // at +/-16384, close to what the vectors use, with 2x headroom left.
    parameter integer RX_SHIFT = 3
) (
    input  wire        clk,
    input  wire        resetn,

    // ADI side (axi_ad9361 adc_* ports, l_clk domain)
    input  wire        adc_valid,
    input  wire        adc_enable_i,
    input  wire        adc_enable_q,
    input  wire [15:0] adc_data_i,
    input  wire [15:0] adc_data_q,

    // AXI4-Stream side
    output reg  [31:0] m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire [3:0]  m_axis_tkeep,
    output wire        m_axis_tlast,

    output reg         overflow
);
    assign m_axis_tkeep = 4'hF;
    assign m_axis_tlast = 1'b0;

    // Both channels must be enabled for the pair to mean anything; if the
    // core has only one channel up, the Q half would be stale rather than
    // merely zero, and a QPSK slicer would happily decode the garbage.
    wire take = adc_valid & adc_enable_i & adc_enable_q;

    // Saturating left shift of each 16-bit sample by RX_SHIFT.
    function [15:0] shl_sat;
        input [15:0] v;
        reg signed [15+RX_SHIFT:0] w;
        begin
            w = $signed(v) <<< RX_SHIFT;
            if (w >  $signed({{RX_SHIFT{1'b0}}, 16'sh7FFF})) shl_sat = 16'sh7FFF;
            else if (w < $signed({{RX_SHIFT{1'b1}}, 16'sh8000})) shl_sat = 16'sh8000;
            else shl_sat = w[15:0];
        end
    endfunction

    wire [15:0] sat_i = shl_sat(adc_data_i);
    wire [15:0] sat_q = shl_sat(adc_data_q);

    always @(posedge clk) begin
        if (!resetn) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= 32'd0;
            overflow      <= 1'b0;
        end else begin
            // A beat already accepted this cycle frees the register.
            if (m_axis_tvalid & m_axis_tready)
                m_axis_tvalid <= 1'b0;

            if (take) begin
                if (m_axis_tvalid & ~m_axis_tready) begin
                    // Sink is stalled and the ADC produced anyway: the new
                    // sample is dropped, and that fact is latched.
                    overflow <= 1'b1;
                end else begin
                    // Shift with saturation: a sample wider than expected
                    // must clip, not wrap. A wrapped sample inverts its sign,
                    // and a QPSK decision is nothing but a pair of sign tests.
                    m_axis_tdata  <= {sat_q, sat_i};
                    m_axis_tvalid <= 1'b1;
                end
            end
        end
    end
endmodule
