// Coherent 64-QAM hard decisions. Exact boundaries choose the upper level.
// Outputs per-symbol squared constellation error (Q4.26) and clipping flag.
// Carrier/timing recovery and gain normalization precede this block.
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
    wire [2:0] bi=index(s_i), bq=index(s_q);
    wire signed [16:0] di = {s_i[15],s_i} - $signed(level(bi));
    wire signed [16:0] dq = {s_q[15],s_q} - $signed(level(bq));
    wire [33:0] ei = di * di, eq = dq * dq;
    assign s_ready = !m_valid || m_ready;
    always @(posedge clk) begin
        if (!resetn) begin m_valid<=0; m_data<=0; m_error<=0; m_clipped<=0; end
        else if (s_ready) begin
            m_valid <= s_valid;
            if (s_valid) begin
                m_data <= {bi ^ (bi >> 1), bq ^ (bq >> 1)};
                m_error <= ei + eq;
                m_clipped <= s_i==32767 || s_i==-32768 || s_q==32767 || s_q==-32768;
            end
        end
    end
endmodule
