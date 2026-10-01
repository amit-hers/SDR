// Systematic RS(255,223) encoder over GF(2^8), primitive polynomial 0x11D
// (x^8+x^4+x^3+x^2+1), primitive element alpha=0x02, first consecutive root
// index fcr=0. Generator polynomial g(x) = prod_{i=0}^{31} (x - alpha^i).
//
// NOT verified bit-exact against the host's liquid-dsp LIQUID_FEC_RS_M8
// (src/core/fec/ReedSolomon.cpp) -- liquid-dsp's internal root/basis
// convention isn't available without its source, which this environment
// does not have (only the compiled library and public header are
// installed). Verified instead against an independent, auditable Python
// reference using the defining property of an RS codeword: every output
// codeword evaluates to zero when evaluated at all 32 generator roots
// (alpha^0 .. alpha^31). This is a correct, standard systematic RS(255,223)
// encoder; it is a separate codec from the host's, not a drop-in
// replacement for interop with it.
//
// Streaming: accepts 223 data bytes (passed through unchanged, systematic),
// then emits 32 parity bytes (highest-degree remainder term first), then
// repeats. m_last marks the final byte (the 255th) of each codeword.
module rs_encoder (
    input wire clk, input wire resetn,
    input wire [7:0] s_data, input wire s_valid, output wire s_ready,
    output reg [7:0] m_data, output reg m_valid, input wire m_ready,
    output reg m_last
);
    localparam integer K    = 223;
    localparam integer NPAR = 32;

    // g[i] = coefficient of x^i in the generator polynomial, i=0..31
    // (g[32]=1, monic, is implicit and not a tap). Computed offline from
    // the GF(2^8) construction above -- see the Python reference.
    reg [7:0] gpoly [0:NPAR-1];
    initial begin
        gpoly[0]=8'h58;  gpoly[1]=8'hAC;  gpoly[2]=8'h37;  gpoly[3]=8'h8E;
        gpoly[4]=8'h14;  gpoly[5]=8'hFD;  gpoly[6]=8'h8A;  gpoly[7]=8'h18;
        gpoly[8]=8'hB9;  gpoly[9]=8'hB3;  gpoly[10]=8'h2F; gpoly[11]=8'h94;
        gpoly[12]=8'hE4; gpoly[13]=8'hFD; gpoly[14]=8'h37; gpoly[15]=8'h3B;
        gpoly[16]=8'h0C; gpoly[17]=8'hE1; gpoly[18]=8'hC5; gpoly[19]=8'hB0;
        gpoly[20]=8'h9D; gpoly[21]=8'h21; gpoly[22]=8'h21; gpoly[23]=8'hA2;
        gpoly[24]=8'hC2; gpoly[25]=8'h10; gpoly[26]=8'h7E; gpoly[27]=8'h36;
        gpoly[28]=8'hAE; gpoly[29]=8'h34; gpoly[30]=8'h40; gpoly[31]=8'h74;
    end

    function [7:0] gf_mul;
        input [7:0] a, b;
        reg [7:0] p, aa, bb;
        integer i;
        begin
            p = 8'h00; aa = a; bb = b;
            for (i = 0; i < 8; i = i + 1) begin
                if (bb[0]) p = p ^ aa;
                bb = bb >> 1;
                aa = aa[7] ? ((aa << 1) ^ 8'h1D) : (aa << 1);
            end
            gf_mul = p;
        end
    endfunction

    reg [7:0] reg_ [0:NPAR-1];   // running remainder, reg_[i] = coeff of x^i
    reg       in_data;           // 1: accepting/passing through data bytes
    reg [4:0] pidx;              // which parity byte to emit next, 31..0
    reg [7:0] dcount;            // data bytes accepted so far in this block

    // Registered output, so "ready to produce a new value this cycle" is
    // !m_valid (empty) or m_ready (downstream takes the current one now).
    wire out_free = !m_valid || m_ready;
    assign s_ready = in_data && out_free;

    wire accept_data = in_data && s_valid && out_free;
    wire [7:0] fb = s_data ^ reg_[NPAR-1];
    integer j;

    always @(posedge clk) begin
        if (!resetn) begin
            m_valid <= 1'b0; m_data <= 8'h00; m_last <= 1'b0;
            in_data <= 1'b1; pidx <= NPAR-1; dcount <= 8'd0;
            for (j = 0; j < NPAR; j = j + 1) reg_[j] <= 8'h00;
        end else if (in_data) begin
            if (accept_data) begin
                m_data  <= s_data;
                m_valid <= 1'b1;
                m_last  <= 1'b0;
                for (j = NPAR-1; j > 0; j = j - 1)
                    reg_[j] <= reg_[j-1] ^ gf_mul(fb, gpoly[j]);
                reg_[0] <= gf_mul(fb, gpoly[0]);
                if (dcount == K-1) begin
                    dcount  <= 8'd0;
                    in_data <= 1'b0;
                    pidx    <= NPAR-1;
                end else begin
                    dcount <= dcount + 8'd1;
                end
            end else if (out_free) begin
                m_valid <= 1'b0;
            end
        end else begin
            // Draining parity bytes reg_[31] .. reg_[0].
            if (out_free) begin
                m_data  <= reg_[pidx];
                m_valid <= 1'b1;
                m_last  <= (pidx == 0);
                if (pidx == 0) begin
                    in_data <= 1'b1;
                    for (j = 0; j < NPAR; j = j + 1) reg_[j] <= 8'h00;
                end else begin
                    pidx <= pidx - 1'b1;
                end
            end
        end
    end
endmodule
