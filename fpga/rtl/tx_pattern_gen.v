// tx_pattern_gen -- TEMPORARY. Feeds qpsk_mod the exact bytes of
// vectors/mod_ref.bits straight from a ROM, bypassing Linux, IIO and the DMA.
//
// Why this exists: every attempt to prove the TX waveform through the byte-DMA
// transport starved the modulator (it emits VALID ZERO samples when its input
// stream is empty -- "if (s_axis_bits.empty()) goto rrc_out"), so the capture
// windows contained startup gaps rather than signal. With the payload in
// fabric the modulator can never starve, which isolates "is the pulse-shaped
// IQ correct" from "does the DMA transport work".
//
// mod_ref.bits is the same 256-byte vector the modulator's C simulation uses,
// so the captured IQ can be correlated against vectors/mod_ref.iq at the bar
// csim passes at. `enable` restarts the sequence at byte 0, so a capture armed
// at the same moment sees the reference from its first sample -- including the
// RRC's zero-state transient, which is exactly how the reference was generated.
module tx_pattern_gen (
    input  wire       clk,
    input  wire       resetn,
    input  wire       enable,          // from the probe GPIO, AXI clock domain

    output wire [7:0] m_axis_tdata,
    output wire       m_axis_tvalid,
    input  wire       m_axis_tready,
    output wire       m_axis_tlast,
    output wire       m_axis_tkeep
);
    reg [7:0] rom [0:255];
    initial begin
        rom[0]=8'h74; rom[1]=8'hD9; rom[2]=8'h67; rom[3]=8'h69; rom[4]=8'h20; rom[5]=8'hFB; rom[6]=8'hC4; rom[7]=8'h68;
        rom[8]=8'hC6; rom[9]=8'h27; rom[10]=8'h67; rom[11]=8'h25; rom[12]=8'h16; rom[13]=8'h0F; rom[14]=8'hE5; rom[15]=8'h65;
        rom[16]=8'h7D; rom[17]=8'h5D; rom[18]=8'h0D; rom[19]=8'h3C; rom[20]=8'hB9; rom[21]=8'hAA; rom[22]=8'h55; rom[23]=8'h75;
        rom[24]=8'hDA; rom[25]=8'h2C; rom[26]=8'hE2; rom[27]=8'hB6; rom[28]=8'hFF; rom[29]=8'h53; rom[30]=8'h92; rom[31]=8'h0D;
        rom[32]=8'hA9; rom[33]=8'h23; rom[34]=8'hCD; rom[35]=8'h35; rom[36]=8'h5F; rom[37]=8'hA4; rom[38]=8'h4F; rom[39]=8'h3F;
        rom[40]=8'h24; rom[41]=8'h29; rom[42]=8'h9A; rom[43]=8'hA7; rom[44]=8'h66; rom[45]=8'h65; rom[46]=8'hEC; rom[47]=8'h00;
        rom[48]=8'h4B; rom[49]=8'h0A; rom[50]=8'hAC; rom[51]=8'hA1; rom[52]=8'h63; rom[53]=8'h20; rom[54]=8'hAA; rom[55]=8'h6F;
        rom[56]=8'h4F; rom[57]=8'hE7; rom[58]=8'hB1; rom[59]=8'hAD; rom[60]=8'hAD; rom[61]=8'h14; rom[62]=8'h0A; rom[63]=8'h47;
        rom[64]=8'hE2; rom[65]=8'hE5; rom[66]=8'h89; rom[67]=8'hCF; rom[68]=8'h2E; rom[69]=8'h29; rom[70]=8'h2B; rom[71]=8'h3A;
        rom[72]=8'h69; rom[73]=8'h26; rom[74]=8'h6A; rom[75]=8'h4E; rom[76]=8'hB5; rom[77]=8'hB8; rom[78]=8'h37; rom[79]=8'hF3;
        rom[80]=8'h7B; rom[81]=8'hAE; rom[82]=8'hD8; rom[83]=8'h0D; rom[84]=8'h7F; rom[85]=8'h1E; rom[86]=8'h96; rom[87]=8'hB3;
        rom[88]=8'hE0; rom[89]=8'h2C; rom[90]=8'h78; rom[91]=8'h10; rom[92]=8'h7E; rom[93]=8'h49; rom[94]=8'hCF; rom[95]=8'hF4;
        rom[96]=8'h6C; rom[97]=8'h87; rom[98]=8'hA1; rom[99]=8'hCD; rom[100]=8'h99; rom[101]=8'hE0; rom[102]=8'h6C; rom[103]=8'hCF;
        rom[104]=8'h7A; rom[105]=8'h99; rom[106]=8'hE7; rom[107]=8'hD3; rom[108]=8'h55; rom[109]=8'h1D; rom[110]=8'h37; rom[111]=8'hBD;
        rom[112]=8'h1D; rom[113]=8'h3B; rom[114]=8'h52; rom[115]=8'h3A; rom[116]=8'h73; rom[117]=8'h7A; rom[118]=8'h25; rom[119]=8'hC6;
        rom[120]=8'hBB; rom[121]=8'hB8; rom[122]=8'h1F; rom[123]=8'h8E; rom[124]=8'hAA; rom[125]=8'h96; rom[126]=8'h31; rom[127]=8'hC4;
        rom[128]=8'h10; rom[129]=8'h61; rom[130]=8'hF5; rom[131]=8'h9A; rom[132]=8'hB2; rom[133]=8'hA5; rom[134]=8'h0F; rom[135]=8'h04;
        rom[136]=8'hC3; rom[137]=8'hCC; rom[138]=8'hF9; rom[139]=8'hAC; rom[140]=8'h4B; rom[141]=8'h4D; rom[142]=8'hE4; rom[143]=8'h0D;
        rom[144]=8'hFD; rom[145]=8'h77; rom[146]=8'h0F; rom[147]=8'h0E; rom[148]=8'h6C; rom[149]=8'h83; rom[150]=8'h93; rom[151]=8'hA9;
        rom[152]=8'h13; rom[153]=8'h7A; rom[154]=8'h76; rom[155]=8'h36; rom[156]=8'h1D; rom[157]=8'hB2; rom[158]=8'hF0; rom[159]=8'h4D;
        rom[160]=8'h6B; rom[161]=8'hF8; rom[162]=8'h8F; rom[163]=8'h5F; rom[164]=8'h7F; rom[165]=8'h79; rom[166]=8'h03; rom[167]=8'hA1;
        rom[168]=8'h87; rom[169]=8'hAC; rom[170]=8'hD1; rom[171]=8'h36; rom[172]=8'hDE; rom[173]=8'h6B; rom[174]=8'h42; rom[175]=8'hBB;
        rom[176]=8'hF5; rom[177]=8'h12; rom[178]=8'h00; rom[179]=8'hB0; rom[180]=8'h30; rom[181]=8'h4F; rom[182]=8'h36; rom[183]=8'h36;
        rom[184]=8'hAD; rom[185]=8'h04; rom[186]=8'h39; rom[187]=8'h8D; rom[188]=8'h7D; rom[189]=8'h07; rom[190]=8'h66; rom[191]=8'h72;
        rom[192]=8'h69; rom[193]=8'h36; rom[194]=8'h34; rom[195]=8'h0B; rom[196]=8'h1D; rom[197]=8'h56; rom[198]=8'h9B; rom[199]=8'h39;
        rom[200]=8'h07; rom[201]=8'h79; rom[202]=8'hCD; rom[203]=8'h1E; rom[204]=8'h6B; rom[205]=8'h6B; rom[206]=8'h7A; rom[207]=8'h37;
        rom[208]=8'hD9; rom[209]=8'hBE; rom[210]=8'h35; rom[211]=8'h03; rom[212]=8'h46; rom[213]=8'h52; rom[214]=8'h2F; rom[215]=8'hCF;
        rom[216]=8'h57; rom[217]=8'hF8; rom[218]=8'h96; rom[219]=8'hF6; rom[220]=8'hBB; rom[221]=8'h25; rom[222]=8'hD2; rom[223]=8'h94;
        rom[224]=8'h84; rom[225]=8'hAE; rom[226]=8'h43; rom[227]=8'h95; rom[228]=8'hC3; rom[229]=8'h57; rom[230]=8'h1A; rom[231]=8'h67;
        rom[232]=8'h43; rom[233]=8'h8A; rom[234]=8'hBD; rom[235]=8'hFA; rom[236]=8'h98; rom[237]=8'h64; rom[238]=8'hB8; rom[239]=8'hC7;
        rom[240]=8'hDC; rom[241]=8'h13; rom[242]=8'hCB; rom[243]=8'hF6; rom[244]=8'h2C; rom[245]=8'hA4; rom[246]=8'hB7; rom[247]=8'h5B;
        rom[248]=8'hF6; rom[249]=8'h24; rom[250]=8'hBD; rom[251]=8'h01; rom[252]=8'h33; rom[253]=8'h5D; rom[254]=8'h84; rom[255]=8'h00;
    end

    // enable crosses from the AXI clock domain
    reg [1:0] en_s;
    always @(posedge clk) en_s <= {en_s[0], enable};
    wire en = en_s[1];

    reg [7:0] addr;
    reg [7:0] dout;

    always @(posedge clk) begin
        if (!resetn || !en) begin
            addr <= 8'd0;                 // disabled: hold at the start
        end else if (m_axis_tvalid && m_axis_tready) begin
            addr <= addr + 8'd1;          // wraps naturally at 256
        end
        dout <= rom[addr];
    end

    // Combinational data straight off the ROM index keeps the byte aligned
    // with `addr`; dout above exists only to let the tool infer block RAM if
    // it prefers to, and is unused for the handshake.
    assign m_axis_tdata  = rom[addr];
    assign m_axis_tvalid = en;
    assign m_axis_tlast  = (addr == 8'd255);
    assign m_axis_tkeep  = 1'b1;
endmodule
