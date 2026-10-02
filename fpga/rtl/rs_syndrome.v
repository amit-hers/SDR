// RS(255,223) syndrome calculator over GF(2^8), companion to rs_encoder.v
// (same primitive polynomial 0x11D, primitive element alpha=0x02, fcr=0).
// Computes S_i = r(alpha^i) for i=0..31 by Horner's method, one running
// accumulator per root, evaluated in parallel as each byte streams in.
//
// First decoder stage only: this calculator reports the 32 syndromes (and
// an all-zero flag for the no-error case) but does not locate or correct
// errors. Berlekamp-Massey, Chien search and Forney are not implemented.
//
// Streaming: accepts 255 received bytes (highest-degree coefficient, i.e.
// the first transmitted byte, first), then emits S_0..S_31 in that order,
// then repeats. m_last marks the final syndrome (S_31) of each block.
// all_zero is valid for one cycle alongside the last syndrome (S_31) and
// means the received word decoded with zero errors -- no residual
// Berlekamp-Massey/correction work needed for that block.
module rs_syndrome (
    input wire clk, input wire resetn,
    input wire [7:0] s_data, input wire s_valid, output wire s_ready,
    output reg [7:0] m_data, output reg m_valid, input wire m_ready,
    output reg m_last, output reg all_zero
);
    localparam integer N    = 255;
    localparam integer NSYN = 32;

    // ROOTPOW[i] = alpha^i, i=0..31 (fcr=0).
    reg [7:0] rootpow [0:NSYN-1];
    initial begin
        rootpow[0]=8'h01;  rootpow[1]=8'h02;  rootpow[2]=8'h04;  rootpow[3]=8'h08;
        rootpow[4]=8'h10;  rootpow[5]=8'h20;  rootpow[6]=8'h40;  rootpow[7]=8'h80;
        rootpow[8]=8'h1D;  rootpow[9]=8'h3A;  rootpow[10]=8'h74; rootpow[11]=8'hE8;
        rootpow[12]=8'hCD; rootpow[13]=8'h87; rootpow[14]=8'h13; rootpow[15]=8'h26;
        rootpow[16]=8'h4C; rootpow[17]=8'h98; rootpow[18]=8'h2D; rootpow[19]=8'h5A;
        rootpow[20]=8'hB4; rootpow[21]=8'h75; rootpow[22]=8'hEA; rootpow[23]=8'hC9;
        rootpow[24]=8'h8F; rootpow[25]=8'h03; rootpow[26]=8'h06; rootpow[27]=8'h0C;
        rootpow[28]=8'h18; rootpow[29]=8'h30; rootpow[30]=8'h60; rootpow[31]=8'hC0;
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

    reg [7:0] acc [0:NSYN-1];    // acc[i] = running Horner evaluation at alpha^i
    reg       in_data;           // 1: accepting received bytes
    reg [4:0] sidx;              // which syndrome to emit next, 0..31
    reg [7:0] dcount;            // bytes accepted so far in this block

    wire out_free = !m_valid || m_ready;
    assign s_ready = in_data && out_free;
    wire accept_data = in_data && s_valid && out_free;

    wire [7:0] syndromes_nonzero = acc[0]|acc[1]|acc[2]|acc[3]|acc[4]|acc[5]|acc[6]|acc[7]|
                              acc[8]|acc[9]|acc[10]|acc[11]|acc[12]|acc[13]|acc[14]|acc[15]|
                              acc[16]|acc[17]|acc[18]|acc[19]|acc[20]|acc[21]|acc[22]|acc[23]|
                              acc[24]|acc[25]|acc[26]|acc[27]|acc[28]|acc[29]|acc[30]|acc[31];

    integer j;

    always @(posedge clk) begin
        if (!resetn) begin
            m_valid <= 1'b0; m_data <= 8'h00; m_last <= 1'b0; all_zero <= 1'b0;
            in_data <= 1'b1; sidx <= 5'd0; dcount <= 8'd0;
            for (j = 0; j < NSYN; j = j + 1) acc[j] <= 8'h00;
        end else if (in_data) begin
            if (accept_data) begin
                m_valid <= 1'b0;   // no output while still consuming a block
                for (j = 0; j < NSYN; j = j + 1)
                    acc[j] <= gf_mul(acc[j], rootpow[j]) ^ s_data;
                if (dcount == N-1) begin
                    dcount  <= 8'd0;
                    in_data <= 1'b0;
                    sidx    <= 5'd0;
                end else begin
                    dcount <= dcount + 8'd1;
                end
            end else if (out_free) begin
                m_valid <= 1'b0;
            end
        end else begin
            // Draining syndromes S_0 .. S_31.
            if (out_free) begin
                m_data   <= acc[sidx];
                m_valid  <= 1'b1;
                m_last   <= (sidx == NSYN-1);
                all_zero <= (sidx == NSYN-1) && !syndromes_nonzero;
                if (sidx == NSYN-1) begin
                    in_data <= 1'b1;
                    for (j = 0; j < NSYN; j = j + 1) acc[j] <= 8'h00;
                end else begin
                    sidx <= sidx + 1'b1;
                end
            end
        end
    end
endmodule
