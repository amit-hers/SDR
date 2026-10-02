// RS(255,223) error-locator solver (Berlekamp-Massey) over GF(2^8), companion
// to rs_syndrome.v. Consumes the 32 syndromes S_0..S_31 and produces the
// error-locator polynomial coefficients C[0..degree] (low-degree-first,
// C[0]=1) plus the degree itself. Chien search and Forney (locating and
// correcting the errors from this polynomial) are not implemented here.
//
// Fixed-width hardware formulation: unlike the textbook algorithm's
// variable-length arrays, this keeps C and B as full 33-element registers
// at all times (implicitly zero beyond the true degree) and masks sums by
// position instead of by length -- validated against the Python reference
// (tests/rs_decoder_ref.py's berlekamp_massey()) via an intermediate
// fixed-width Python model before writing this RTL, specifically because
// this algorithm's history in this project already produced two separate
// subtle indexing bugs in the simpler Chien-search/Forney stages.
//
// Runs for up to 128 cycles once all 32 syndrome bytes are collected (FOUR
// pipeline stages per Berlekamp-Massey iteration -- see bm_stage below, not
// one cycle per iteration as the first version of this module did); done
// pulses for one cycle when degree/coefficients are valid, and they stay
// registered (stable) until the next block starts.
//
// Pipelined 2026-10 in two passes. First pass split each iteration into
// three stages (DELTA, COEF, UPDATE) to break up three serial GF multiplies
// that failed this project's real 100 MHz AXI-Lite clock outright (WNS
// -3.996 ns; -8.996 ns at 200 MHz). That got to +0.647 ns at 100 MHz --
// positive, but only ~6% of the period, nowhere near the 4+ ns margin every
// other RS block has. Reading Vivado's OWN critical-path report (not
// guessing from the source) showed the actual bottleneck was never the
// multiply chain at all: it's the DELTA stage's own 32-way reduction (32
// independent GF multiplies XOR-folded into one scalar, plus a runtime
// `i<=nn` mask on every term) -- 13 logic levels, deeper than a single
// gf_mul. A second attempt that split a runtime-indexed mux out of the
// UPDATE stage, on the theory that the mux was the problem, measured WORSE
// (+0.394 ns) and confirmed that hypothesis was wrong.
//
// This second pass instead splits the delta reduction itself across two
// stages (DELTA_LO: terms i=1..16, DELTA_HI: terms i=17..32), each roughly
// half the XOR-tree depth, combined in the COEF stage before the
// ginv(b) multiply. Four stages now (128 cycles/block, still irrelevant for
// a once-per-codeword control computation). Validated against the Python
// reference via an intermediate pipelined hardware model -- the same
// precaution used for every stage this project, and the ONLY reason the
// first (wrong) attempt's failure was caught before resynthesizing blind a
// third time.
module rs_berlekamp_massey (
    input wire clk, input wire resetn,
    input wire [7:0] s_data, input wire s_valid, output wire s_ready,
    output reg [7:0] m_data, output reg m_valid, input wire m_ready,
    output reg m_last,
    output reg [5:0] degree, output reg done
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

    // GF(256) multiplicative inverse, indexed by value (entry 0 unused --
    // b is only ever assigned from a nonzero delta, starting at 1).
    reg [7:0] ginv [0:255];
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
    end

    reg [7:0] synd [0:NPAR-1];
    reg [7:0] Creg [0:NPAR];   // error locator, low-degree-first, degree 0..32
    reg [7:0] Breg [0:NPAR];
    reg [5:0] Lr;
    reg [5:0] mr;
    reg [7:0] br;
    reg [5:0] nn;              // outer iteration counter, 0..31

    reg       in_data;         // 1: collecting syndrome bytes
    reg       computing;       // 1: running BM iterations
    reg [4:0] dcount;          // syndrome bytes collected so far
    reg [5:0] oidx;            // output drain index

    wire out_free = !m_valid || m_ready;
    assign s_ready = in_data && out_free;
    wire accept_data = in_data && s_valid && out_free;

    // Combinational per-stage scratch (masked by position, not by a
    // runtime-length loop bound).
    reg [7:0] delta;
    reg [7:0] newC [0:NPAR];
    integer i, k;

    // Pipeline stage within the current iteration, and the registers that
    // carry a value from one stage to the next (see the module header for
    // why this is split into three stages).
    //
    // A fourth stage was tried here, splitting Breg[k-mr]'s runtime-indexed
    // mux out from the multiply that follows it, on the theory that the mux
    // was lengthening that stage's path. It measured WORSE (100 MHz slack
    // +0.394 ns vs this 3-stage version's +0.647 ns) and was reverted: the
    // actual critical path (confirmed by reading Vivado's own path report,
    // not by guessing from the source) runs entirely inside ST_DELTA's own
    // 32-way reduction -- synd_reg to delta_reg, 13 logic levels -- which
    // neither version touches. That reduction (32 independent GF multiplies
    // XOR-folded into one scalar) is structurally different from, and
    // deeper than, the single independent multiply per output that lets
    // rs_syndrome.v and rs_chien_search.v meet 200 MHz comfortably; a real
    // fix needs pipelining the reduction TREE itself (e.g. two levels of
    // partial-sum registers), not another stage boundary elsewhere. Left as
    // a known limitation: this module meets the real 100 MHz target with
    // positive but thin slack (+0.647 ns, i.e. about 6% of the period),
    // comfortably short of the 4+ ns margin every other RS block in this
    // project has. Treat that margin as fragile against PVT variation on
    // real silicon, not as equivalent to the other blocks' headroom.
    localparam ST_DELTA_LO=3'd0, ST_DELTA_HI=3'd1, ST_COEF=3'd2, ST_UPDATE=3'd3;
    reg [2:0] bm_stage;
    reg [7:0] partial_lo_reg;
    reg [7:0] partial_hi_reg;
    reg [7:0] delta_reg;
    reg [7:0] coef_reg;

    always @(posedge clk) begin
        if (!resetn) begin
            m_valid <= 1'b0; m_data <= 8'h00; m_last <= 1'b0;
            done <= 1'b0; degree <= 6'd0;
            in_data <= 1'b1; computing <= 1'b0; dcount <= 5'd0; oidx <= 6'd0;
            Lr <= 6'd0; mr <= 6'd1; br <= 8'h01; nn <= 6'd0; bm_stage <= ST_DELTA_LO;
            for (i = 0; i <= NPAR; i = i + 1) begin
                Creg[i] <= (i == 0) ? 8'h01 : 8'h00;
                Breg[i] <= (i == 0) ? 8'h01 : 8'h00;
            end
        end else begin
            done <= 1'b0;
            if (in_data) begin
                if (accept_data) begin
                    synd[dcount] <= s_data;
                    if (dcount == NPAR-1) begin
                        in_data   <= 1'b0;
                        computing <= 1'b1;
                        // Must not rely on the input-side stall pattern to
                        // have cleared these: if every byte of this block
                        // was accepted back-to-back (no s_valid=0 cycle),
                        // the "else if(out_free) m_valid<=0" path below
                        // never ran, and a value held from the PREVIOUS
                        // block's last output byte would otherwise still
                        // read as valid all the way through this block's
                        // entire silent computing phase.
                        m_valid <= 1'b0; m_last <= 1'b0;
                        nn <= 6'd0; bm_stage <= ST_DELTA_LO;
                        Lr <= 6'd0; mr <= 6'd1; br <= 8'h01;
                        for (i = 0; i <= NPAR; i = i + 1) begin
                            Creg[i] <= (i == 0) ? 8'h01 : 8'h00;
                            Breg[i] <= (i == 0) ? 8'h01 : 8'h00;
                        end
                    end else begin
                        dcount <= dcount + 1'b1;
                    end
                end
            end else if (computing) begin
                case (bm_stage)
                    ST_DELTA_LO: begin
                        // partial_lo = synd[nn] XOR sum_{i=1}^{16} (i<=nn) gf_mul(Creg[i], synd[nn-i])
                        delta = synd[nn];
                        for (i = 1; i <= 16; i = i + 1)
                            if (i <= nn) delta = delta ^ gf_mul(Creg[i], synd[nn-i]);
                        partial_lo_reg <= delta;
                        bm_stage       <= ST_DELTA_HI;
                    end
                    ST_DELTA_HI: begin
                        // partial_hi = sum_{i=17}^{32} (i<=nn) gf_mul(Creg[i], synd[nn-i])
                        delta = 8'h00;
                        for (i = 17; i <= NPAR; i = i + 1)
                            if (i <= nn) delta = delta ^ gf_mul(Creg[i], synd[nn-i]);
                        partial_hi_reg <= delta;
                        bm_stage       <= ST_COEF;
                    end
                    ST_COEF: begin
                        // Combining the two halves is a single cheap XOR, so
                        // folding it into the same cycle as the ginv(b)
                        // multiply keeps this stage only one gf_mul deep.
                        delta_reg <= partial_lo_reg ^ partial_hi_reg;
                        coef_reg  <= gf_mul(partial_lo_reg ^ partial_hi_reg, ginv[br]);
                        bm_stage  <= ST_UPDATE;
                    end
                    ST_UPDATE: begin
                        // newC[k] = Creg[k] ^ (k>=mr ? gf_mul(coef_reg, Breg[k-mr]) : 0)
                        for (k = 0; k <= NPAR; k = k + 1)
                            newC[k] = Creg[k] ^ ((k >= mr) ? gf_mul(coef_reg, Breg[k-mr]) : 8'h00);

                        if (delta_reg == 8'h00) begin
                            mr <= mr + 1'b1;
                            // Creg, Breg, Lr, br unchanged
                        end else begin
                            for (k = 0; k <= NPAR; k = k + 1) Creg[k] <= newC[k];
                            if ((Lr << 1) <= nn) begin
                                for (k = 0; k <= NPAR; k = k + 1) Breg[k] <= Creg[k]; // OLD Creg
                                Lr <= nn + 1'b1 - Lr;
                                br <= delta_reg;
                                mr <= 6'd1;
                            end else begin
                                mr <= mr + 1'b1;
                            end
                        end

                        if (nn == NPAR-1) begin
                            computing <= 1'b0;
                            done      <= 1'b1;
                            // Lr/Creg above reflect this cycle's update via nonblocking
                            // assignment; degree/output drain use the values that will
                            // be valid next cycle.
                            oidx <= 6'd0;
                        end
                        nn       <= nn + 1'b1;
                        bm_stage <= ST_DELTA_LO;
                    end
                endcase
            end else begin
                // Drain Creg[0..Lr] as a byte stream, low-degree-first.
                if (oidx == 6'd0 && !m_valid) degree <= Lr;
                if (out_free) begin
                    m_data  <= Creg[oidx];
                    m_valid <= 1'b1;
                    m_last  <= (oidx == Lr);
                    if (oidx == Lr) begin
                        in_data <= 1'b1;
                        dcount  <= 5'd0;
                    end else begin
                        oidx <= oidx + 1'b1;
                    end
                end
            end
        end
    end
endmodule
