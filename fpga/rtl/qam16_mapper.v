// Gray-labelled square 16-QAM, coherent (non-differential).
// Each axis is Gray(ascending binary index), levels -3,-1,+1,+3.
// Q2.13 samples: round(8192/sqrt(10)) = 2591 per level; mean energy ~1.
// Same normalization convention as qam64_mapper.v: base = round(8192 /
// sqrt(2*(M-1)/3)), M=16 here vs M=64 there.
// The upstream framer must keep acquisition/header in a robust modulation.
module qam16_mapper (
    input wire clk, input wire resetn,
    input wire [3:0] s_data, input wire s_valid, output wire s_ready,
    output reg signed [15:0] m_i, output reg signed [15:0] m_q,
    output reg m_valid, input wire m_ready
);
    function signed [15:0] amplitude;
        input [1:0] gray;
        reg [1:0] binary;
        begin
            binary[1] = gray[1];
            binary[0] = gray[1] ^ gray[0];
            case (binary)
                0: amplitude = -7773;
                1: amplitude = -2591;
                2: amplitude = 2591;
                3: amplitude = 7773;
            endcase
        end
    endfunction
    assign s_ready = !m_valid || m_ready;
    always @(posedge clk) begin
        if (!resetn) begin m_valid <= 0; m_i <= 0; m_q <= 0; end
        else if (s_ready) begin
            m_valid <= s_valid;
            if (s_valid) begin m_i <= amplitude(s_data[3:2]); m_q <= amplitude(s_data[1:0]); end
        end
    end
endmodule
