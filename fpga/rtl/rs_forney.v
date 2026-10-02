// RS(255,223) Forney algorithm over GF(2^8), the last RS(255,223) decoder
// stage: given the error-locator coefficients, the syndromes, and the error
// DEGREES rs_chien_search.v found, compute the error MAGNITUDE at each one --
// i.e. actually correct the errors, not just locate them.
//
// Protocol, in three phases:
//   1. COEF:  accept the locator C[0..degree] (low-degree-first, as
//             rs_berlekamp_massey.v emits it) on cs_data, c_last marking the
//             final coefficient byte. Zero-padded internally to 33 entries,
//             same convention as rs_chien_search.v.
//   2. SYND:  immediately afterward, accept the 32 syndromes (as
//             rs_syndrome.v emits them) on the SAME cs_data bus, cs_last
//             marking the final (32nd) syndrome byte.
//   3. READY: once both are in, Omega(x) = S(x)*C(x) mod x^32 is computed
//             internally (32 cycles), then the block accepts error-degree
//             requests on d_data (one byte each, from rs_chien_search.v's
//             own output stream) and emits one (position, magnitude) pair
//             per request on pos_data/mag_data, 32 internal cycles per
//             request (two parallel Horner evaluations: Omega and the
//             locator's formal derivative, both at alpha^{-j}).
//
// forney_fail pulses (alongside a still-asserted m_valid) if a particular
// request's Forney denominator was zero -- a real decode failure for THAT
// position (uncorrectable), not reset here; system integration decides how
// to handle it (e.g. discard the whole block).
//
// Validated against the Python reference (forney() in tests/rs_decoder_ref.py)
// via an intermediate fixed-width Python hardware model before writing this
// RTL -- the same precaution that caught bugs in every earlier stage this
// session.
module rs_forney (
    input wire clk, input wire resetn,
    input wire [7:0] cs_data, input wire cs_valid, output wire cs_ready,
    input wire c_last, input wire cs_last,
    input wire [7:0] d_data, input wire d_valid, output wire d_ready,
    output reg [7:0] pos_data, output reg [7:0] mag_data,
    output reg m_valid, input wire m_ready,
    output reg forney_fail
);
    localparam integer NPAR = 32;

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

    // Same two lookup tables as rs_berlekamp_massey.v (ginv) and a new one
    // this stage needs on its own: alpha^j for ARBITRARY j=0..254 (Chien
    // search and BM only ever need per-REGISTER-POSITION constants i=0..32,
    // but Forney needs alpha^{-j} for the actual found DEGREE j, which
    // ranges over all 255 positions) -- addr=255-j, with entry 255 a
    // duplicate of entry 0 (alpha^0=1=alpha^255) so j=0 needs no special case.
    reg [7:0] ginv [0:255];
    reg [7:0] alphapow [0:255];
    initial begin
        ginv[0]=8'h00; ginv[1]=8'h01; ginv[2]=8'h8E; ginv[3]=8'hF4; ginv[4]=8'h47; ginv[5]=8'hA7; ginv[6]=8'h7A; ginv[7]=8'hBA;
        ginv[8]=8'hAD; ginv[9]=8'h9D; ginv[10]=8'hDD; ginv[11]=8'h98; ginv[12]=8'h3D; ginv[13]=8'hAA; ginv[14]=8'h5D; ginv[15]=8'h96;
        ginv[16]=8'hD8; ginv[17]=8'h72; ginv[18]=8'hC0; ginv[19]=8'h58; ginv[20]=8'hE0; ginv[21]=8'h3E; ginv[22]=8'h4C; ginv[23]=8'h66;
        ginv[24]=8'h90; ginv[25]=8'hDE; ginv[26]=8'h55; ginv[27]=8'h80; ginv[28]=8'hA0; ginv[29]=8'h83; ginv[30]=8'h4B; ginv[31]=8'h2A;
        ginv[32]=8'h6C; ginv[33]=8'hED; ginv[34]=8'h39; ginv[35]=8'h51; ginv[36]=8'h60; ginv[37]=8'h56; ginv[38]=8'h2C; ginv[39]=8'h8A;
        ginv[40]=8'h70; ginv[41]=8'hD0; ginv[42]=8'h1F; ginv[43]=8'h4A; ginv[44]=8'h26; ginv[45]=8'h8B; ginv[46]=8'h33; ginv[47]=8'h6E;
        ginv[48]=8'h48; ginv[49]=8'h89; ginv[50]=8'h6F; ginv[51]=8'h2E; ginv[52]=8'hA4; ginv[53]=8'hC3; ginv[54]=8'h40; ginv[55]=8'h5E;
        ginv[56]=8'h50; ginv[57]=8'h22; ginv[58]=8'hCF; ginv[59]=8'hA9; ginv[60]=8'hAB; ginv[61]=8'h0C; ginv[62]=8'h15; ginv[63]=8'hE1;
        ginv[64]=8'h36; ginv[65]=8'h5F; ginv[66]=8'hF8; ginv[67]=8'hD5; ginv[68]=8'h92; ginv[69]=8'h4E; ginv[70]=8'hA6; ginv[71]=8'h04;
        ginv[72]=8'h30; ginv[73]=8'h88; ginv[74]=8'h2B; ginv[75]=8'h1E; ginv[76]=8'h16; ginv[77]=8'h67; ginv[78]=8'h45; ginv[79]=8'h93;
        ginv[80]=8'h38; ginv[81]=8'h23; ginv[82]=8'h68; ginv[83]=8'h8C; ginv[84]=8'h81; ginv[85]=8'h1A; ginv[86]=8'h25; ginv[87]=8'h61;
        ginv[88]=8'h13; ginv[89]=8'hC1; ginv[90]=8'hCB; ginv[91]=8'h63; ginv[92]=8'h97; ginv[93]=8'h0E; ginv[94]=8'h37; ginv[95]=8'h41;
        ginv[96]=8'h24; ginv[97]=8'h57; ginv[98]=8'hCA; ginv[99]=8'h5B; ginv[100]=8'hB9; ginv[101]=8'hC4; ginv[102]=8'h17; ginv[103]=8'h4D;
        ginv[104]=8'h52; ginv[105]=8'h8D; ginv[106]=8'hEF; ginv[107]=8'hB3; ginv[108]=8'h20; ginv[109]=8'hEC; ginv[110]=8'h2F; ginv[111]=8'h32;
        ginv[112]=8'h28; ginv[113]=8'hD1; ginv[114]=8'h11; ginv[115]=8'hD9; ginv[116]=8'hE9; ginv[117]=8'hFB; ginv[118]=8'hDA; ginv[119]=8'h79;
        ginv[120]=8'hDB; ginv[121]=8'h77; ginv[122]=8'h06; ginv[123]=8'hBB; ginv[124]=8'h84; ginv[125]=8'hCD; ginv[126]=8'hFE; ginv[127]=8'hFC;
        ginv[128]=8'h1B; ginv[129]=8'h54; ginv[130]=8'hA1; ginv[131]=8'h1D; ginv[132]=8'h7C; ginv[133]=8'hCC; ginv[134]=8'hE4; ginv[135]=8'hB0;
        ginv[136]=8'h49; ginv[137]=8'h31; ginv[138]=8'h27; ginv[139]=8'h2D; ginv[140]=8'h53; ginv[141]=8'h69; ginv[142]=8'h02; ginv[143]=8'hF5;
        ginv[144]=8'h18; ginv[145]=8'hDF; ginv[146]=8'h44; ginv[147]=8'h4F; ginv[148]=8'h9B; ginv[149]=8'hBC; ginv[150]=8'h0F; ginv[151]=8'h5C;
        ginv[152]=8'h0B; ginv[153]=8'hDC; ginv[154]=8'hBD; ginv[155]=8'h94; ginv[156]=8'hAC; ginv[157]=8'h09; ginv[158]=8'hC7; ginv[159]=8'hA2;
        ginv[160]=8'h1C; ginv[161]=8'h82; ginv[162]=8'h9F; ginv[163]=8'hC6; ginv[164]=8'h34; ginv[165]=8'hC2; ginv[166]=8'h46; ginv[167]=8'h05;
        ginv[168]=8'hCE; ginv[169]=8'h3B; ginv[170]=8'h0D; ginv[171]=8'h3C; ginv[172]=8'h9C; ginv[173]=8'h08; ginv[174]=8'hBE; ginv[175]=8'hB7;
        ginv[176]=8'h87; ginv[177]=8'hE5; ginv[178]=8'hEE; ginv[179]=8'h6B; ginv[180]=8'hEB; ginv[181]=8'hF2; ginv[182]=8'hBF; ginv[183]=8'hAF;
        ginv[184]=8'hC5; ginv[185]=8'h64; ginv[186]=8'h07; ginv[187]=8'h7B; ginv[188]=8'h95; ginv[189]=8'h9A; ginv[190]=8'hAE; ginv[191]=8'hB6;
        ginv[192]=8'h12; ginv[193]=8'h59; ginv[194]=8'hA5; ginv[195]=8'h35; ginv[196]=8'h65; ginv[197]=8'hB8; ginv[198]=8'hA3; ginv[199]=8'h9E;
        ginv[200]=8'hD2; ginv[201]=8'hF7; ginv[202]=8'h62; ginv[203]=8'h5A; ginv[204]=8'h85; ginv[205]=8'h7D; ginv[206]=8'hA8; ginv[207]=8'h3A;
        ginv[208]=8'h29; ginv[209]=8'h71; ginv[210]=8'hC8; ginv[211]=8'hF6; ginv[212]=8'hF9; ginv[213]=8'h43; ginv[214]=8'hD7; ginv[215]=8'hD6;
        ginv[216]=8'h10; ginv[217]=8'h73; ginv[218]=8'h76; ginv[219]=8'h78; ginv[220]=8'h99; ginv[221]=8'h0A; ginv[222]=8'h19; ginv[223]=8'h91;
        ginv[224]=8'h14; ginv[225]=8'h3F; ginv[226]=8'hE6; ginv[227]=8'hF0; ginv[228]=8'h86; ginv[229]=8'hB1; ginv[230]=8'hE2; ginv[231]=8'hF1;
        ginv[232]=8'hFA; ginv[233]=8'h74; ginv[234]=8'hF3; ginv[235]=8'hB4; ginv[236]=8'h6D; ginv[237]=8'h21; ginv[238]=8'hB2; ginv[239]=8'h6A;
        ginv[240]=8'hE3; ginv[241]=8'hE7; ginv[242]=8'hB5; ginv[243]=8'hEA; ginv[244]=8'h03; ginv[245]=8'h8F; ginv[246]=8'hD3; ginv[247]=8'hC9;
        ginv[248]=8'h42; ginv[249]=8'hD4; ginv[250]=8'hE8; ginv[251]=8'h75; ginv[252]=8'h7F; ginv[253]=8'hFF; ginv[254]=8'h7E; ginv[255]=8'hFD;

        alphapow[0]=8'h01;  alphapow[1]=8'h02;  alphapow[2]=8'h04;  alphapow[3]=8'h08;  alphapow[4]=8'h10;  alphapow[5]=8'h20;  alphapow[6]=8'h40;  alphapow[7]=8'h80;
        alphapow[8]=8'h1D;  alphapow[9]=8'h3A;  alphapow[10]=8'h74; alphapow[11]=8'hE8; alphapow[12]=8'hCD; alphapow[13]=8'h87; alphapow[14]=8'h13; alphapow[15]=8'h26;
        alphapow[16]=8'h4C; alphapow[17]=8'h98; alphapow[18]=8'h2D; alphapow[19]=8'h5A; alphapow[20]=8'hB4; alphapow[21]=8'h75; alphapow[22]=8'hEA; alphapow[23]=8'hC9;
        alphapow[24]=8'h8F; alphapow[25]=8'h03; alphapow[26]=8'h06; alphapow[27]=8'h0C; alphapow[28]=8'h18; alphapow[29]=8'h30; alphapow[30]=8'h60; alphapow[31]=8'hC0;
        alphapow[32]=8'h9D; alphapow[33]=8'h27; alphapow[34]=8'h4E; alphapow[35]=8'h9C; alphapow[36]=8'h25; alphapow[37]=8'h4A; alphapow[38]=8'h94; alphapow[39]=8'h35;
        alphapow[40]=8'h6A; alphapow[41]=8'hD4; alphapow[42]=8'hB5; alphapow[43]=8'h77; alphapow[44]=8'hEE; alphapow[45]=8'hC1; alphapow[46]=8'h9F; alphapow[47]=8'h23;
        alphapow[48]=8'h46; alphapow[49]=8'h8C; alphapow[50]=8'h05; alphapow[51]=8'h0A; alphapow[52]=8'h14; alphapow[53]=8'h28; alphapow[54]=8'h50; alphapow[55]=8'hA0;
        alphapow[56]=8'h5D; alphapow[57]=8'hBA; alphapow[58]=8'h69; alphapow[59]=8'hD2; alphapow[60]=8'hB9; alphapow[61]=8'h6F; alphapow[62]=8'hDE; alphapow[63]=8'hA1;
        alphapow[64]=8'h5F; alphapow[65]=8'hBE; alphapow[66]=8'h61; alphapow[67]=8'hC2; alphapow[68]=8'h99; alphapow[69]=8'h2F; alphapow[70]=8'h5E; alphapow[71]=8'hBC;
        alphapow[72]=8'h65; alphapow[73]=8'hCA; alphapow[74]=8'h89; alphapow[75]=8'h0F; alphapow[76]=8'h1E; alphapow[77]=8'h3C; alphapow[78]=8'h78; alphapow[79]=8'hF0;
        alphapow[80]=8'hFD; alphapow[81]=8'hE7; alphapow[82]=8'hD3; alphapow[83]=8'hBB; alphapow[84]=8'h6B; alphapow[85]=8'hD6; alphapow[86]=8'hB1; alphapow[87]=8'h7F;
        alphapow[88]=8'hFE; alphapow[89]=8'hE1; alphapow[90]=8'hDF; alphapow[91]=8'hA3; alphapow[92]=8'h5B; alphapow[93]=8'hB6; alphapow[94]=8'h71; alphapow[95]=8'hE2;
        alphapow[96]=8'hD9; alphapow[97]=8'hAF; alphapow[98]=8'h43; alphapow[99]=8'h86; alphapow[100]=8'h11; alphapow[101]=8'h22; alphapow[102]=8'h44; alphapow[103]=8'h88;
        alphapow[104]=8'h0D; alphapow[105]=8'h1A; alphapow[106]=8'h34; alphapow[107]=8'h68; alphapow[108]=8'hD0; alphapow[109]=8'hBD; alphapow[110]=8'h67; alphapow[111]=8'hCE;
        alphapow[112]=8'h81; alphapow[113]=8'h1F; alphapow[114]=8'h3E; alphapow[115]=8'h7C; alphapow[116]=8'hF8; alphapow[117]=8'hED; alphapow[118]=8'hC7; alphapow[119]=8'h93;
        alphapow[120]=8'h3B; alphapow[121]=8'h76; alphapow[122]=8'hEC; alphapow[123]=8'hC5; alphapow[124]=8'h97; alphapow[125]=8'h33; alphapow[126]=8'h66; alphapow[127]=8'hCC;
        alphapow[128]=8'h85; alphapow[129]=8'h17; alphapow[130]=8'h2E; alphapow[131]=8'h5C; alphapow[132]=8'hB8; alphapow[133]=8'h6D; alphapow[134]=8'hDA; alphapow[135]=8'hA9;
        alphapow[136]=8'h4F; alphapow[137]=8'h9E; alphapow[138]=8'h21; alphapow[139]=8'h42; alphapow[140]=8'h84; alphapow[141]=8'h15; alphapow[142]=8'h2A; alphapow[143]=8'h54;
        alphapow[144]=8'hA8; alphapow[145]=8'h4D; alphapow[146]=8'h9A; alphapow[147]=8'h29; alphapow[148]=8'h52; alphapow[149]=8'hA4; alphapow[150]=8'h55; alphapow[151]=8'hAA;
        alphapow[152]=8'h49; alphapow[153]=8'h92; alphapow[154]=8'h39; alphapow[155]=8'h72; alphapow[156]=8'hE4; alphapow[157]=8'hD5; alphapow[158]=8'hB7; alphapow[159]=8'h73;
        alphapow[160]=8'hE6; alphapow[161]=8'hD1; alphapow[162]=8'hBF; alphapow[163]=8'h63; alphapow[164]=8'hC6; alphapow[165]=8'h91; alphapow[166]=8'h3F; alphapow[167]=8'h7E;
        alphapow[168]=8'hFC; alphapow[169]=8'hE5; alphapow[170]=8'hD7; alphapow[171]=8'hB3; alphapow[172]=8'h7B; alphapow[173]=8'hF6; alphapow[174]=8'hF1; alphapow[175]=8'hFF;
        alphapow[176]=8'hE3; alphapow[177]=8'hDB; alphapow[178]=8'hAB; alphapow[179]=8'h4B; alphapow[180]=8'h96; alphapow[181]=8'h31; alphapow[182]=8'h62; alphapow[183]=8'hC4;
        alphapow[184]=8'h95; alphapow[185]=8'h37; alphapow[186]=8'h6E; alphapow[187]=8'hDC; alphapow[188]=8'hA5; alphapow[189]=8'h57; alphapow[190]=8'hAE; alphapow[191]=8'h41;
        alphapow[192]=8'h82; alphapow[193]=8'h19; alphapow[194]=8'h32; alphapow[195]=8'h64; alphapow[196]=8'hC8; alphapow[197]=8'h8D; alphapow[198]=8'h07; alphapow[199]=8'h0E;
        alphapow[200]=8'h1C; alphapow[201]=8'h38; alphapow[202]=8'h70; alphapow[203]=8'hE0; alphapow[204]=8'hDD; alphapow[205]=8'hA7; alphapow[206]=8'h53; alphapow[207]=8'hA6;
        alphapow[208]=8'h51; alphapow[209]=8'hA2; alphapow[210]=8'h59; alphapow[211]=8'hB2; alphapow[212]=8'h79; alphapow[213]=8'hF2; alphapow[214]=8'hF9; alphapow[215]=8'hEF;
        alphapow[216]=8'hC3; alphapow[217]=8'h9B; alphapow[218]=8'h2B; alphapow[219]=8'h56; alphapow[220]=8'hAC; alphapow[221]=8'h45; alphapow[222]=8'h8A; alphapow[223]=8'h09;
        alphapow[224]=8'h12; alphapow[225]=8'h24; alphapow[226]=8'h48; alphapow[227]=8'h90; alphapow[228]=8'h3D; alphapow[229]=8'h7A; alphapow[230]=8'hF4; alphapow[231]=8'hF5;
        alphapow[232]=8'hF7; alphapow[233]=8'hF3; alphapow[234]=8'hFB; alphapow[235]=8'hEB; alphapow[236]=8'hCB; alphapow[237]=8'h8B; alphapow[238]=8'h0B; alphapow[239]=8'h16;
        alphapow[240]=8'h2C; alphapow[241]=8'h58; alphapow[242]=8'hB0; alphapow[243]=8'h7D; alphapow[244]=8'hFA; alphapow[245]=8'hE9; alphapow[246]=8'hCF; alphapow[247]=8'h83;
        alphapow[248]=8'h1B; alphapow[249]=8'h36; alphapow[250]=8'h6C; alphapow[251]=8'hD8; alphapow[252]=8'hAD; alphapow[253]=8'h47; alphapow[254]=8'h8E; alphapow[255]=8'h01;
    end

    reg [7:0] Creg [0:NPAR];      // locator coefficients, zero-padded
    reg [7:0] Sreg [0:NPAR-1];    // syndromes
    reg [7:0] omega [0:NPAR-1];   // error evaluator, dense, index==degree

    // Phase tracking.
    localparam ST_COEF=0, ST_SYND=1, ST_OMEGA=2, ST_READY=3, ST_EVAL=4, ST_FINISH=5;
    reg [2:0] state;
    reg [5:0] cidx;    // 0..32 while collecting C
    reg [5:0] sidx;    // 0..31 while collecting S
    reg [5:0] oi;      // 0..31 during Omega convolution
    reg [5:0] ei;      // 31 downto 0 during Horner evaluation
    reg [7:0] req_j;
    reg [7:0] x_inv;        // alpha^{-req_j}, latched for the whole EVAL pass
    reg [7:0] r_omega, r_cp; // Horner accumulators

    wire out_free = !m_valid || m_ready;
    // ST_READY accepts cs_valid too: it's how a new block's collection
    // phase starts (see the ST_READY case below -- there is no other path
    // back from serving degree requests to loading a fresh C/S pair).
    assign cs_ready = (state == ST_COEF || state == ST_SYND || state == ST_READY);
    assign d_ready  = (state == ST_READY) && out_free;

    // Formal derivative of the locator, dense: Cp[k] = C[k+1] if k even, 0 if
    // k odd (the char-2 formal derivative drops every even-power term and
    // keeps odd-power ones shifted down by one degree). Pure wiring.
    wire [7:0] Cp [0:NPAR-1];
    genvar gk;
    generate
        for (gk = 0; gk < NPAR; gk = gk + 1) begin : CP_WIRE
            assign Cp[gk] = (gk[0] == 1'b0) ? Creg[gk+1] : 8'h00;
        end
    endgenerate

    integer i;

    always @(posedge clk) begin
        if (!resetn) begin
            state <= ST_COEF; cidx <= 6'd0; sidx <= 6'd0; oi <= 6'd0; ei <= 6'd0;
            m_valid <= 1'b0; forney_fail <= 1'b0; pos_data <= 8'h00; mag_data <= 8'h00;
            for (i = 0; i <= NPAR; i = i + 1) Creg[i] <= 8'h00;
            for (i = 0; i < NPAR; i = i + 1) Sreg[i] <= 8'h00;
            for (i = 0; i < NPAR; i = i + 1) omega[i] <= 8'h00;
        end else begin
            forney_fail <= 1'b0;
            case (state)
                ST_COEF: begin
                    if (cs_valid) begin
                        Creg[cidx] <= cs_data;
                        if (c_last) begin
                            state <= ST_SYND; sidx <= 6'd0;
                        end else begin
                            cidx <= cidx + 1'b1;
                        end
                    end
                end
                ST_SYND: begin
                    if (cs_valid) begin
                        Sreg[sidx] <= cs_data;
                        if (cs_last) begin
                            state <= ST_OMEGA; oi <= 6'd0;
                            for (i = 0; i < NPAR; i = i + 1) omega[i] <= 8'h00;
                        end else begin
                            sidx <= sidx + 1'b1;
                        end
                    end
                end
                ST_OMEGA: begin
                    // omega[k] ^= Sreg[oi] * Creg[k-oi] for every k >= oi.
                    for (i = 0; i < NPAR; i = i + 1)
                        if (i >= oi)
                            omega[i] <= omega[i] ^ gf_mul(Sreg[oi], Creg[i-oi]);
                    if (oi == NPAR-1) state <= ST_READY;
                    else oi <= oi + 1'b1;
                end
                ST_READY: begin
                    if (m_valid && m_ready) m_valid <= 1'b0;
                    // A new block's coefficients starting to arrive takes
                    // priority: there is otherwise no path back to ST_COEF,
                    // so without this the module would serve only the FIRST
                    // block ever loaded. Clear the old block's higher-order
                    // Creg/Sreg/omega entries here -- a shorter polynomial in
                    // the new block must not see the previous block's stale
                    // high-degree coefficients.
                    if (cs_valid) begin
                        Creg[0] <= cs_data;
                        for (i = 1; i <= NPAR; i = i + 1) Creg[i] <= 8'h00;
                        for (i = 0; i < NPAR; i = i + 1) Sreg[i]  <= 8'h00;
                        for (i = 0; i < NPAR; i = i + 1) omega[i] <= 8'h00;
                        if (c_last) begin
                            state <= ST_SYND; sidx <= 6'd0;
                        end else begin
                            state <= ST_COEF; cidx <= 6'd1;
                        end
                    end else if (d_ready && d_valid) begin
                        req_j   <= d_data;
                        x_inv   <= alphapow[8'hFF - d_data];
                        r_omega <= 8'h00; r_cp <= 8'h00;
                        ei      <= 6'd31;
                        state   <= ST_EVAL;
                    end
                end
                ST_EVAL: begin
                    // Horner, highest degree (31) down to 0, both polys in
                    // lockstep using the same x_inv. The updated r_omega/r_cp
                    // from processing index 0 are only visible NEXT cycle
                    // (nonblocking assignment), so the division that needs
                    // the completed result happens in ST_FINISH, one cycle
                    // after this reaches ei==0.
                    r_omega <= gf_mul(r_omega, x_inv) ^ omega[ei];
                    r_cp    <= gf_mul(r_cp,    x_inv) ^ Cp[ei];
                    if (ei == 6'd0) state <= ST_FINISH;
                    else             ei    <= ei - 1'b1;
                end
                ST_FINISH: begin
                    // r_omega/r_cp now hold Omega(x_inv) and Cp(x_inv).
                    pos_data <= req_j;
                    m_valid  <= 1'b1;
                    if (r_cp == 8'h00) begin
                        forney_fail <= 1'b1;
                        mag_data    <= 8'h00;
                    end else begin
                        mag_data <= gf_mul(alphapow[req_j], gf_mul(r_omega, ginv[r_cp]));
                    end
                    state <= ST_READY;
                end
            endcase
        end
    end
endmodule
