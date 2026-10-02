// RS(255,223) Chien search over GF(2^8), companion to rs_berlekamp_massey.v.
// Consumes the error-locator coefficients C[0..degree] (low-degree-first, as
// rs_berlekamp_massey.v emits them -- s_last here is meant to be wired
// directly to that module's m_last) and emits the DEGREE of each position
// j=0..254 where C(alpha^{-j})==0, i.e. each error location, as a byte
// stream. search_done pulses once after all 255 positions have been tested.
//
// Architecture: one register per coefficient position (33 total, matching
// rs_berlekamp_massey.v's fixed-width convention -- zero-padded beyond the
// true degree, so no separate degree input is needed here). Each register i
// carries a FIXED per-position multiplier alpha^{-i}; every cycle its value
// is C[i]*alpha^{-i*j} for the current j, so XOR-reducing all 33 tests
// position j directly, and advancing every register by its own constant
// moves to j+1. Validated against the Python reference (chien_search() in
// tests/rs_decoder_ref.py) via an intermediate fixed-width Python model
// before writing this RTL -- the same precaution that caught two real bugs
// in the Berlekamp-Massey stage.
//
// Forney (computing the actual error MAGNITUDE at each located position) is
// not implemented here -- this block only locates errors, it does not
// correct them.
module rs_chien_search (
    input wire clk, input wire resetn,
    input wire [7:0] s_data, input wire s_valid, output wire s_ready, input wire s_last,
    output reg [7:0] m_data, output reg m_valid, input wire m_ready,
    output reg search_done
);
    localparam integer NPAR = 32;

    reg [7:0] alphainv [0:NPAR];
    initial begin
        alphainv[0]=8'h01;  alphainv[1]=8'h8E;  alphainv[2]=8'h47;  alphainv[3]=8'hAD;
        alphainv[4]=8'hD8;  alphainv[5]=8'h6C;  alphainv[6]=8'h36;  alphainv[7]=8'h1B;
        alphainv[8]=8'h83;  alphainv[9]=8'hCF;  alphainv[10]=8'hE9; alphainv[11]=8'hFA;
        alphainv[12]=8'h7D; alphainv[13]=8'hB0; alphainv[14]=8'h58; alphainv[15]=8'h2C;
        alphainv[16]=8'h16; alphainv[17]=8'h0B; alphainv[18]=8'h8B; alphainv[19]=8'hCB;
        alphainv[20]=8'hEB; alphainv[21]=8'hFB; alphainv[22]=8'hF3; alphainv[23]=8'hF7;
        alphainv[24]=8'hF5; alphainv[25]=8'hF4; alphainv[26]=8'h7A; alphainv[27]=8'h3D;
        alphainv[28]=8'h90; alphainv[29]=8'h48; alphainv[30]=8'h24; alphainv[31]=8'h12;
        alphainv[32]=8'h09;
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

    reg [7:0] reg_ [0:NPAR];     // reg_[i] = C[i] * alpha^{-i*j} for current j
    reg       in_data;           // 1: collecting coefficient bytes
    reg       searching;         // 1: running the 255-position search
    reg [5:0] cidx;              // next coefficient slot to fill, 0..32
    reg [7:0] jj;                // current test position, 0..254

    wire out_free = !m_valid || m_ready;
    assign s_ready = in_data && out_free;
    wire accept_data = in_data && s_valid && out_free;

    wire [7:0] xor_reduce = reg_[0]^reg_[1]^reg_[2]^reg_[3]^reg_[4]^reg_[5]^reg_[6]^reg_[7]^
                       reg_[8]^reg_[9]^reg_[10]^reg_[11]^reg_[12]^reg_[13]^reg_[14]^reg_[15]^
                       reg_[16]^reg_[17]^reg_[18]^reg_[19]^reg_[20]^reg_[21]^reg_[22]^reg_[23]^
                       reg_[24]^reg_[25]^reg_[26]^reg_[27]^reg_[28]^reg_[29]^reg_[30]^reg_[31]^
                       reg_[32];
    // A found root is pending output exactly when searching and the current
    // (not-yet-advanced) register state reduces to zero.
    wire root_here = searching && (xor_reduce == 8'h00);

    integer i;

    always @(posedge clk) begin
        if (!resetn) begin
            m_valid <= 1'b0; m_data <= 8'h00; search_done <= 1'b0;
            in_data <= 1'b1; searching <= 1'b0; cidx <= 6'd0; jj <= 8'd0;
            for (i = 0; i <= NPAR; i = i + 1) reg_[i] <= 8'h00;
        end else begin
            search_done <= 1'b0;
            if (in_data) begin
                if (accept_data) begin
                    reg_[cidx] <= s_data;
                    if (s_last) begin
                        in_data   <= 1'b0;
                        searching <= 1'b1;
                        jj        <= 8'd0;
                        cidx      <= 6'd0;
                    end else begin
                        cidx <= cidx + 1'b1;
                    end
                end else if (out_free) begin
                    m_valid <= 1'b0;
                end
            end else if (searching) begin
                // Gate EVERY advance (root or not) on out_free, not just a
                // root's own output cycle: the first draft advanced through
                // non-root positions at full speed regardless of whether a
                // PREVIOUS root's output byte had actually been consumed,
                // which could silently overwrite (lose) it under
                // backpressure. Only ever move past position jj once
                // whatever was sitting in m_data/m_valid has been drained.
                if (out_free) begin
                    m_valid <= root_here;
                    m_data  <= jj;   // don't-care when root_here is 0
                    if (jj == 8'd254) begin
                        searching   <= 1'b0;
                        in_data     <= 1'b1;
                        search_done <= 1'b1;
                        cidx        <= 6'd0;
                        for (i = 0; i <= NPAR; i = i + 1) reg_[i] <= 8'h00;
                    end else begin
                        for (i = 0; i <= NPAR; i = i + 1)
                            reg_[i] <= gf_mul(reg_[i], alphainv[i]);
                        jj <= jj + 1'b1;
                    end
                end
            end
        end
    end
endmodule
