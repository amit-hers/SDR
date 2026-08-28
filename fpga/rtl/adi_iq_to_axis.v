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
module adi_iq_to_axis (
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
                    m_axis_tdata  <= {adc_data_q, adc_data_i};
                    m_axis_tvalid <= 1'b1;
                end
            end
        end
    end
endmodule
