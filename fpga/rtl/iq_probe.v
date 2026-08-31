// iq_probe -- TEMPORARY diagnostic tap on the TX IQ stream.
//
// Spliced between tx_iq_cc/M_AXIS and axis_to_iq/s_axis to answer one
// question: does the modulator actually put correctly shaped QPSK/RRC IQ in
// front of the adapter? The demodulated loopback was all zeros while the TX
// DMA reported completed transfers, and those two facts cannot both describe a
// working TX path.
//
// PASS-THROUGH, NOT A BROADCASTER. Every stream signal is wired straight
// across combinationally, so inserting this cannot change the handshake. A
// broadcaster would gate TREADY on the probe also being ready, which would
// backpressure the TX stream and manufacture the underflow this is trying to
// measure.
//
// Readout deliberately avoids a second AXI slave: capture lands in an inferred
// block RAM and is read one word at a time through an axi_gpio, addr in /
// data out. Slow (two devmem calls per sample) but it needs no BRAM-interface
// wiring in the block design.
//
//   ctrl[31]    arm    -- rising edge starts a one-shot capture
//   ctrl[30]    sel    -- 1: stat returns the captured count instead of data
//   ctrl[15:0]  raddr  -- word to present on stat
//   stat[31:0]         -- {Q[15:0], I[15:0]} at raddr, or the count when sel=1
module iq_probe #(
    parameter integer DEPTH_LOG2 = 11          // 2048 samples = 512 QPSK symbols
) (
    input  wire        clk,                    // l_clk: the M_AXIS side
    input  wire        resetn,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,
    input  wire [3:0]  s_axis_tkeep,

    output wire [31:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,
    output wire [3:0]  m_axis_tkeep,

    input  wire [31:0] ctrl,                   // AXI clock domain
    output wire [31:0] stat
);
    localparam integer DEPTH = (1 << DEPTH_LOG2);

    assign m_axis_tdata  = s_axis_tdata;
    assign m_axis_tvalid = s_axis_tvalid;
    assign m_axis_tlast  = s_axis_tlast;
    assign m_axis_tkeep  = s_axis_tkeep;
    assign s_axis_tready = m_axis_tready;

    wire beat = s_axis_tvalid & m_axis_tready;

    // ctrl crosses from the AXI clock. The WHOLE bus must be synchronised,
    // not just the arm bit: the read address feeds the block RAM's address
    // port directly, and leaving it unsynchronised put 23 clk_fpga_0 -> rx_clk
    // endpoints into the timing graph and failed the design at WNS -2.856 ns.
    // Bus skew across the two flops is harmless here because ctrl is held
    // static by software while a word is read back.
    reg [31:0] ctrl_s1, ctrl_s2;
    always @(posedge clk) begin
        ctrl_s1 <= ctrl;
        ctrl_s2 <= ctrl_s1;
    end
    wire        arm   = ctrl_s2[31];
    wire        sel   = ctrl_s2[30];
    wire [DEPTH_LOG2-1:0] raddr = ctrl_s2[DEPTH_LOG2-1:0];

    reg [1:0] arm_s;
    always @(posedge clk) arm_s <= {arm_s[0], arm};

    (* ram_style = "block" *) reg [31:0] mem [0:DEPTH-1];
    reg [DEPTH_LOG2-1:0] waddr;
    reg                  running;
    reg [31:0]           rdata;

    always @(posedge clk) begin
        if (!resetn) begin
            waddr   <= {DEPTH_LOG2{1'b0}};
            running <= 1'b0;
        end else if (!arm_s[1]) begin           // disarmed: ready to re-arm
            waddr   <= {DEPTH_LOG2{1'b0}};
            running <= 1'b0;
        end else if (!running && waddr == {DEPTH_LOG2{1'b0}}) begin
            running <= 1'b1;                    // armed, start
        end else if (running && beat) begin
            mem[waddr] <= s_axis_tdata;
            if (&waddr) running <= 1'b0;        // buffer full: stop, hold data
            waddr <= waddr + 1'b1;
        end
        rdata <= mem[raddr];
    end

    // Readback is stable once capture has stopped, so no sync is needed here.
    assign stat = sel ? {{(32-DEPTH_LOG2-1){1'b0}}, running, waddr} : rdata;
endmodule
