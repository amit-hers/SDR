// Synchronous capture of the AD9363 RX data port, in the DATA_CLK domain.
//
// The asynchronous GPIO probe proved WHICH 14 balls the chip drives, but it
// cannot recover bit ORDER: samples taken over AXI are unrelated to DATA_CLK,
// so a PN pattern just looks like noise. This captures RX_FRAME + the 12
// confirmed data lanes on consecutive DATA_CLK edges into a BRAM, which the PS
// then reads over AXI. With the AD9363 in BIST PRBS the captured lanes carry a
// deterministic LFSR, so the lane->bit permutation can be SOLVED rather than
// guessed.
//
// `arm` crosses from the AXI clock and is synchronised with two flops. Capture
// runs once per arming: raise arm, wait for done, read the BRAM, lower arm.
// One-shot (rather than free-running) is what makes the readback coherent --
// the writer is stopped before the PS reads the other port.
module pn_capture #(
  parameter DEPTH_LOG2 = 10,
  parameter ADDR_W     = 12
) (
  input  wire                clk,      // DATA_CLK (N20), via BUFG
  input  wire                arm,      // from axi_gpio, AXI clock domain
  input  wire                frame,    // RX_FRAME (U18)
  input  wire [11:0]         data,     // 12 confirmed RX data balls
  output reg                 we,
  output reg  [ADDR_W-1:0]   addr,
  output reg  [31:0]         din,
  output reg                 done
);
  reg [1:0]            arm_s;
  reg [DEPTH_LOG2-1:0] cnt;
  reg                  run;

  always @(posedge clk) begin
    arm_s <= {arm_s[0], arm};

    if (!arm_s[1]) begin              // disarmed: idle and ready to re-arm
      run  <= 1'b0;
      done <= 1'b0;
      we   <= 1'b0;
      cnt  <= {DEPTH_LOG2{1'b0}};
    end else if (!run && !done) begin // armed, not yet started
      run  <= 1'b1;
      we   <= 1'b0;
      cnt  <= {DEPTH_LOG2{1'b0}};
    end else if (run) begin
      we   <= 1'b1;
      addr <= {{(ADDR_W-DEPTH_LOG2-2){1'b0}}, cnt, 2'b00};  // byte address
      din  <= {19'd0, frame, data};
      cnt  <= cnt + 1'b1;
      if (&cnt) begin                 // last sample of the buffer
        run  <= 1'b0;
        done <= 1'b1;
      end
    end else begin
      we   <= 1'b0;
    end
  end
endmodule
