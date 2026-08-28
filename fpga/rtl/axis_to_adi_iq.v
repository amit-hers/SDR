// axis_to_adi_iq -- AXI4-Stream (32-bit, I in [15:0])  ->  ADI parallel IQ.
//
// The mirror of adi_iq_to_axis, and the direction of the handshake is the
// point: axi_ad9361 drives dac_valid_i0 as a REQUEST. When it is high the core
// samples dac_data_i0/q0 on that edge, and it will do so whether or not the
// stream has anything to give. So dac_valid becomes TREADY, not TVALID.
//
// When the stream is dry on a request cycle the DAC is fed zeros -- silence,
// not the previous sample repeated, which would put a spurious tone on air --
// and `underflow` latches so the condition is visible instead of merely
// audible.
module axis_to_adi_iq (
    input  wire        clk,
    input  wire        resetn,

    // AXI4-Stream side
    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    // ADI side (axi_ad9361 dac_* ports, l_clk domain)
    input  wire        dac_valid,
    input  wire        dac_enable_i,
    input  wire        dac_enable_q,
    output reg  [15:0] dac_data_i,
    output reg  [15:0] dac_data_q,

    output reg         underflow
);
    // One beat consumed per DAC request. Nothing is buffered: adding a skid
    // buffer here would only move the underflow, not remove it, because the
    // request rate is set by the converter and cannot be slowed down.
    assign s_axis_tready = dac_valid & dac_enable_i & dac_enable_q;

    always @(posedge clk) begin
        if (!resetn) begin
            dac_data_i <= 16'd0;
            dac_data_q <= 16'd0;
            underflow  <= 1'b0;
        end else if (s_axis_tready) begin
            if (s_axis_tvalid) begin
                dac_data_i <= s_axis_tdata[15:0];
                dac_data_q <= s_axis_tdata[31:16];
            end else begin
                dac_data_i <= 16'd0;
                dac_data_q <= 16'd0;
                underflow  <= 1'b1;
            end
        end
    end
endmodule
