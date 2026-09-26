// Gray-labelled square 64-QAM, coherent (non-differential).
// Each axis is Gray(ascending binary index), levels -7,-5,-3,-1,+1,+3,+5,+7.
// Q2.13 samples: round(8192/sqrt(42)) = 1264 per level; mean energy ~1.
// The upstream framer must keep acquisition/header in a robust modulation.
module qam64_mapper (
    input wire clk, input wire resetn,
    input wire [5:0] s_data, input wire s_valid, output wire s_ready,
    output reg signed [15:0] m_i, output reg signed [15:0] m_q,
    output reg m_valid, input wire m_ready
);
    function signed [15:0] amplitude;
        input [2:0] gray;
        reg [2:0] binary;
        begin
            binary[2] = gray[2];
            binary[1] = gray[2] ^ gray[1];
            binary[0] = gray[2] ^ gray[1] ^ gray[0];
            case (binary)
                0: amplitude = -8848;
                1: amplitude = -6320;
                2: amplitude = -3792;
                3: amplitude = -1264;
                4: amplitude = 1264;
                5: amplitude = 3792;
                6: amplitude = 6320;
                7: amplitude = 8848;
            endcase
        end
    endfunction
    assign s_ready = !m_valid || m_ready;
    always @(posedge clk) begin
        if (!resetn) begin m_valid <= 0; m_i <= 0; m_q <= 0; end
        else if (s_ready) begin
            m_valid <= s_valid;
            if (s_valid) begin m_i <= amplitude(s_data[5:3]); m_q <= amplitude(s_data[2:0]); end
        end
    end
endmodule
