// dac_pin_probe -- TEMPORARY diagnostic tap on the DAC's PARALLEL pins.
//
// Every probe in this design taps an AXI4-Stream, and all of them sit UPSTREAM
// of axis_to_adi_iq. That leaves one span uninstrumented -- axis_to_adi_iq's
// output to axi_ad9361's dac_data_i0/q0 -- and it is exactly the span the
// evidence points at: the TX IQ probe correlates 1.000000 with mod_ref.iq while
// a second radio shows the air carrying QPSK whose symbols match the payload at
// no alignment. Structurally, no existing probe can see the difference.
//
// This samples the parallel bus on the same qualifier the converter uses
// (dac_valid), so the capture is exactly the sequence of words the AD9363 is
// handed. Purely observational: no output drives anything.
//
// Control/readback is the iq_probe protocol, unchanged, because it is already
// proven on this board:
//   ctrl[31]   arm   -- rising edge starts a one-shot capture
//   ctrl[30]   sel   -- 1: stat returns status instead of data
//   ctrl[15:0] raddr -- word to present on stat
//   stat       {dac_data_q, dac_data_i} at raddr, or {done,running,waddr}
module dac_pin_probe #(
    parameter integer DEPTH_LOG2 = 11          // 2048 samples = 512 QPSK symbols
) (
    input  wire        clk,                    // l_clk: the converter's domain
    input  wire        resetn,

    input  wire [15:0] dac_data_i,
    input  wire [15:0] dac_data_q,
    input  wire        dac_valid,

    input  wire [31:0] ctrl,                   // AXI clock domain
    output wire [31:0] stat
);
    localparam integer DEPTH = (1 << DEPTH_LOG2);

    // The whole ctrl bus is synchronised, not just the arm bit: raddr feeds the
    // block RAM address port directly, and leaving it unsynchronised put 23
    // cross-domain endpoints into the timing graph on the TX probe and failed
    // the design at WNS -2.856 ns. Bus skew is harmless because software holds
    // ctrl static while a word is read back.
    reg [31:0] ctrl_s1, ctrl_s2;
    always @(posedge clk) begin
        ctrl_s1 <= ctrl;
        ctrl_s2 <= ctrl_s1;
    end
    wire                  arm   = ctrl_s2[31];
    wire                  sel   = ctrl_s2[30];
    wire [DEPTH_LOG2-1:0] raddr = ctrl_s2[DEPTH_LOG2-1:0];

    reg [1:0] arm_s;
    always @(posedge clk) arm_s <= {arm_s[0], arm};

    (* ram_style = "block" *) reg [31:0] mem [0:DEPTH-1];
    reg [DEPTH_LOG2-1:0] waddr;
    reg                  running;
    reg                  done;      // latched full; blocks the restart below
    reg [31:0]           rdata;

    always @(posedge clk) begin
        if (!resetn) begin
            waddr   <= {DEPTH_LOG2{1'b0}};
            running <= 1'b0;
            done    <= 1'b0;
        end else if (!arm_s[1]) begin           // disarmed: ready to re-arm
            waddr   <= {DEPTH_LOG2{1'b0}};
            running <= 1'b0;
            done    <= 1'b0;
        end else if (!running && !done) begin
            waddr   <= {DEPTH_LOG2{1'b0}};
            running <= 1'b1;                    // armed, start
        end else if (running && dac_valid) begin
            mem[waddr] <= {dac_data_q, dac_data_i};
            // A full buffer must latch DONE. Without it waddr wraps to 0 and
            // "stopped at 0" is indistinguishable from "armed", so the one-shot
            // restarts forever and every readback blends different passes --
            // the bug that held the TX probe's correlation at chance.
            if (&waddr) begin
                running <= 1'b0;
                done    <= 1'b1;
            end
            waddr <= waddr + 1'b1;
        end
        rdata <= mem[raddr];
    end

    // Readback is stable once capture has stopped, so no sync is needed here.
    assign stat = sel ? {{(32-DEPTH_LOG2-2){1'b0}}, done, running, waddr} : rdata;
endmodule
