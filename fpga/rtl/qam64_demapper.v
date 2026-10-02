// Coherent 64-QAM hard decisions. Exact boundaries choose the upper level.
// Outputs per-symbol squared constellation error (Q4.26) and clipping flag.
// Carrier/timing recovery and gain normalization precede this block.
//
// Pipelined 2026-10-02 to meet this project's real 100 MHz AXI-Lite clock.
// The original single-stage version (index -> level lookup -> subtract ->
// square -> sum, all in one cycle) measured WNS -0.396 ns at 100 MHz (32
// failing endpoints) when exercised in a real AXI-Lite test harness
// (axi_qam64_test_harness_hw.v) -- its 8-way index() comparator is deeper
// than qam16_demapper.v's 4-way one, enough extra depth to turn what is
// otherwise a near-identical squared-error DSP48E1 multiply path into a
// real failure (16-QAM's equivalent path passes, +0.183 ns). Split into two
// stages: stage 1 computes index/level/diff (and the clipping flag) into
// `mid_*` registers; stage 2 consumes those registered values to compute
// the squared-error multiply/sum and the Gray-decoded symbol. Both stages
// share one stall signal (the existing `s_ready = !m_valid || m_ready`,
// unchanged) rather than independent per-stage buffering -- the same
// "freeze everything together" idiom already used throughout this project's
// single-register blocks, just now covering two registers instead of one.
// Latency is 2 cycles instead of 1; throughput is still one symbol in, one
// result out, per cycle, as long as the consumer keeps accepting.
//
// Validated against a Python model of this exact pipeline (500 trials,
// exhaustive constellation points plus random noise and clipping extremes,
// randomized backpressure) before writing this RTL -- the same precaution
// used for every pipelining change in this project (see
// rs_berlekamp_massey.v's history for why that precaution matters).
module qam64_demapper (
    input wire clk, input wire resetn,
    input wire signed [15:0] s_i, input wire signed [15:0] s_q,
    input wire s_valid, output wire s_ready,
    output reg [5:0] m_data, output reg [31:0] m_error,
    output reg m_clipped, output reg m_valid, input wire m_ready
);
    function [2:0] index;
        input signed [15:0] x;
        begin
            if      (x < -7584) index=0;
            else if (x < -5056) index=1;
            else if (x < -2528) index=2;
            else if (x <     0) index=3;
            else if (x <  2528) index=4;
            else if (x <  5056) index=5;
            else if (x <  7584) index=6;
            else               index=7;
        end
    endfunction
    function signed [15:0] level;
        input [2:0] b;
        begin
            case (b)
                0: level=-8848; 1: level=-6320; 2: level=-3792; 3: level=-1264;
                4: level=1264; 5: level=3792; 6: level=6320; 7: level=8848;
            endcase
        end
    endfunction

    // Stage 1 (combinational): index/level/diff/clip from the raw input.
    wire [2:0] bi_c = index(s_i);
    wire [2:0] bq_c = index(s_q);
    wire signed [16:0] di_c = {s_i[15],s_i} - $signed(level(bi_c));
    wire signed [16:0] dq_c = {s_q[15],s_q} - $signed(level(bq_c));
    wire clipped_c = s_i==32767 || s_i==-32768 || s_q==32767 || s_q==-32768;

    // Stage 1 -> stage 2 pipeline register.
    reg        mid_valid;
    reg [2:0]  mid_bi, mid_bq;
    reg signed [16:0] mid_di, mid_dq;
    reg        mid_clipped;

    // Stage 2 (combinational): squared error from the REGISTERED stage-1
    // outputs only -- this is what removes the index/level/subtract chain
    // from the multiply's input cone, the actual fix for the timing gap.
    wire [33:0] ei = mid_di * mid_di;
    wire [33:0] eq = mid_dq * mid_dq;

    assign s_ready = !m_valid || m_ready;
    always @(posedge clk) begin
        if (!resetn) begin
            mid_valid <= 1'b0; mid_bi <= 3'd0; mid_bq <= 3'd0;
            mid_di <= 17'sd0; mid_dq <= 17'sd0; mid_clipped <= 1'b0;
            m_valid <= 1'b0; m_data <= 6'd0; m_error <= 32'd0; m_clipped <= 1'b0;
        end else if (s_ready) begin
            // Stage 2: drain stage 1's registered result into the output.
            m_valid <= mid_valid;
            if (mid_valid) begin
                m_data    <= {mid_bi ^ (mid_bi >> 1), mid_bq ^ (mid_bq >> 1)};
                m_error   <= ei + eq;
                m_clipped <= mid_clipped;
            end
            // Stage 1: capture the new input into the mid registers.
            mid_valid <= s_valid;
            if (s_valid) begin
                mid_bi <= bi_c; mid_bq <= bq_c;
                mid_di <= di_c; mid_dq <= dq_c;
                mid_clipped <= clipped_c;
            end
        end
    end
endmodule
