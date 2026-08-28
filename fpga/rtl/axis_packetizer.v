// axis_packetizer -- insert TLAST every PKT_BYTES beats on a byte stream.
//
// AXI DMA in simple (non-scatter-gather) S2MM mode ends a transfer on TLAST.
// The demodulator cannot supply one: it emits a byte whenever four symbols
// have been sliced, and nothing in a continuous receive stream tells it where
// a buffer should end. Without a TLAST the DMA either never completes or
// completes only on its length counter, which leaves the two sides
// disagreeing about where a frame started.
//
// So the boundary is imposed here, on the BYTE stream, where it costs one
// counter -- rather than on the IQ stream, where a boundary would have to
// survive the demodulator's 4:1 symbol packing and its variable startup
// latency to still land on a byte edge.
//
// PKT_BYTES must match the DMA's per-transfer length. A mismatch is the
// classic cause of a receive path that works for exactly one buffer.
module axis_packetizer #(
    parameter integer PKT_BYTES = 1024
) (
    input  wire       clk,
    input  wire       resetn,

    input  wire [7:0] s_axis_tdata,
    input  wire       s_axis_tvalid,
    output wire       s_axis_tready,
    input  wire       s_axis_tlast,

    output wire [7:0] m_axis_tdata,
    output wire       m_axis_tvalid,
    input  wire       m_axis_tready,
    output wire       m_axis_tkeep,
    output wire       m_axis_tlast
);
    localparam integer CW = (PKT_BYTES <= 1) ? 1 : $clog2(PKT_BYTES);

    reg [CW-1:0] count;
    wire         beat = m_axis_tvalid & m_axis_tready;
    wire         eop  = (count == PKT_BYTES[CW-1:0] - 1'b1);

    // Combinational pass-through: this adds framing, not latency. A register
    // stage here would need a skid buffer to keep TREADY from becoming
    // combinationally dependent on the sink, and it buys nothing.
    assign m_axis_tdata  = s_axis_tdata;
    assign m_axis_tvalid = s_axis_tvalid;
    assign s_axis_tready = m_axis_tready;
    assign m_axis_tkeep  = 1'b1;
    // An upstream TLAST is honoured as well as generated, so a core that does
    // know where its frame ends can still say so.
    assign m_axis_tlast  = s_axis_tlast | eop;

    always @(posedge clk) begin
        if (!resetn)          count <= {CW{1'b0}};
        else if (beat)        count <= (m_axis_tlast) ? {CW{1'b0}} : count + 1'b1;
    end
endmodule
