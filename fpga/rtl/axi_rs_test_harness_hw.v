// Hardware build variant of axi_rs_test_harness.v: encoder + syndrome
// calculator + Chien search + Forney + Berlekamp-Massey -- all five RS
// decoder blocks, now that BM has been pipelined to actually meet this
// peripheral's clock (see below).
//
// Why BM was dropped in rounds 1-3 and is included now: this peripheral's
// AXI-Lite clock is fixed by the project's PS7 configuration at 100 MHz
// (FCLK_CLK0, matching every other AXI-Lite peripheral here -- see
// fpga/bd/sdr_version_id.tcl's s_axi_aclk connection). rs_berlekamp_massey's
// ORIGINAL single-cycle-per-iteration design did not meet that clock
// (WNS -3.996 ns at 100 MHz). It has since been pipelined in two passes
// (three stages, then four -- splitting its delta-reduction tree itself,
// not just adding stage boundaries -- see rs_berlekamp_massey.v's own
// header for the full history) and now measures WNS +2.593 ns at 100 MHz,
// a comfortable, PVT-robust margin -- so it's included here for the first
// hardware test of this block.
//
// Forney meets 100 MHz with +4.3 ns to spare; its interface needs TWO input
// phases (locator coefficients, then syndromes -- two different "last"
// markers) plus a THIRD independent degree-request stream, and produces TWO
// output bytes (position, magnitude) per request instead of one.
//
// Register map (byte offsets, 32-bit registers):
//   0x00 MAGIC       RO  0x52535448 ("RSTH")
//   0x04 SELECT      RW  0=encoder 1=syndrome 2=chien_search 3=forney 4=bm.
//                        Any write also pulses that block's own reset.
//   0x08 PUSH        WO  wdata[7:0] -> selected block's data input, valid
//                        pulsed for one cycle, no "last" marker. For Forney
//                        this feeds cs_data (use for every coefficient/
//                        syndrome byte EXCEPT the two final ones below). For
//                        BM this feeds the 32 syndrome bytes S_0..S_31 (BM
//                        has no "last" marker -- it fixed-counts to 32).
//                        Check STATUS.s_ready first -- a push issued while
//                        not ready is lost.
//   0x0C STATUS      RO  bit0 s_ready (enc/synd/chien/bm) or cs_ready
//                            (forney, ready for more coefficient/syndrome
//                            bytes)
//                        bit1 m_valid (enc/synd/chien/bm) or Forney's own
//                            m_valid (a position/magnitude pair is ready)
//                        bit2 m_last (encoder/syndrome/bm, marks the final
//                            output byte of the CURRENT stream) or
//                            search_done (chien, LATCHED) or 0 (forney)
//                        bit3 d_ready (forney only: ready for a degree
//                            request) -- 0 for every other SELECT
//                        bit4 all_zero (syndrome only)
//                        bit5 forney_fail (forney only: the last magnitude
//                            request hit a zero Forney denominator)
//                        bit6 bm_done_latched (BM only: computation finished
//                            and degree/coefficients are valid -- wait for
//                            this BEFORE reading DEGREE or polling bit1/
//                            popping coefficient bytes. LATCHED, see below.
//                            Meaningless for every other SELECT.)
//   0x10 POP         RO  returns m_data; this SAME read also pulses m_ready
//                        for one cycle (fetch-and-acknowledge in one
//                        transaction). Encoder/syndrome/chien/bm only (bm:
//                        pops one error-locator coefficient byte, low-
//                        degree-first, per read -- stop once bit2 m_last
//                        was seen on the byte just popped).
//   0x14 PUSH_LAST   WO  same as PUSH, but ALSO asserts the "first phase
//                        ended" marker for that byte: s_last for Chien
//                        search's locator input, c_last (end of locator
//                        coefficients, NOT yet the syndromes) for Forney.
//                        Meaningless for BM (push all 32 syndrome bytes via
//                        plain PUSH instead).
//   0x18 PUSH_CS_LAST WO Forney only: same as PUSH, but asserts cs_last --
//                        the FINAL (32nd) syndrome byte, ending the whole
//                        coefficients phase and starting Omega computation.
//   0x1C PUSH_DEGREE WO  Forney only: wdata[7:0] -> d_data, d_valid pulsed
//                        for one cycle. Check STATUS.d_ready first.
//   0x20 POP_POS     RO  Forney only: returns the requested degree
//                        unchanged (pos_data). Read-only peek, no side
//                        effect -- read this BEFORE POP_MAG.
//   0x24 POP_MAG     RO  Forney only: returns the computed error magnitude
//                        (mag_data); this SAME read pulses Forney's own
//                        m_ready, consuming the (position, magnitude) pair
//                        and allowing the next degree request to be
//                        accepted. Meaningless if forney_fail was set for
//                        this request (den==0, uncorrectable).
//   0x28 DEGREE      RO  BM only: the error-locator polynomial's degree
//                        (0-32). Valid only once bit6 bm_done_latched is
//                        set; read-only peek, no side effect.
//
// search_done (chien) and done (bm) are each a ONE-CYCLE pulse in the
// underlying RTL -- a software poll loop over AXI-Lite reads has no
// guaranteed chance of sampling either on that exact cycle. Both latched
// here (chien_done_latched / bm_done_latched), cleared on the next SELECT
// write that resets that block.
module axi_rs_test_harness_hw (
    input  wire        s_axi_aclk,
    input  wire        s_axi_aresetn,

    input  wire [5:0]  s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,

    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,

    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,

    input  wire [5:0]  s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,

    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready
);
    localparam [31:0] MAGIC = 32'h52535448;

    reg [2:0] select;          // 0=encoder, 1=syndrome, 2=chien, 3=forney, 4=bm
    reg       block_resetn;
    reg       do_reset_pulse;

    reg  [7:0] push_data;
    reg        push_valid_pulse;   // -> PUSH
    reg        push_last_pulse;    // -> PUSH_LAST (chien s_last / forney c_last)
    reg        push_cs_last_pulse; // -> PUSH_CS_LAST (forney cs_last)
    reg        push_degree_pulse;  // -> PUSH_DEGREE (forney d_valid)

    wire [7:0] enc_m_data,   synd_m_data,   chien_m_data;
    wire       enc_m_valid,  synd_m_valid,  chien_m_valid;
    wire       enc_m_last,   synd_m_last;
    wire       enc_s_ready,  synd_s_ready,  chien_s_ready;
    wire       synd_all_zero;
    wire       chien_search_done;
    reg        chien_done_latched;

    wire       forney_cs_ready, forney_d_ready, forney_m_valid, forney_fail;
    wire [7:0] forney_pos_data, forney_mag_data;

    wire [7:0] bm_m_data;
    wire       bm_m_valid, bm_m_last, bm_s_ready, bm_done;
    wire [5:0] bm_degree;
    reg        bm_done_latched;

    reg        enc_m_ready_r, synd_m_ready_r, chien_m_ready_r, bm_m_ready_r;
    reg        enc_s_valid_r, synd_s_valid_r, chien_s_valid_r, bm_s_valid_r;
    reg        chien_s_last_r;
    reg        forney_cs_valid_r, forney_c_last_r, forney_cs_last_r;
    reg        forney_d_valid_r, forney_m_ready_r;
    reg        pop_ready_pulse;      // -> POP (enc/synd/chien/bm)
    reg        pop_mag_ready_pulse;  // -> POP_MAG (forney)

    rs_encoder u_enc (
        .clk(s_axi_aclk), .resetn(block_resetn),
        .s_data(push_data), .s_valid(enc_s_valid_r), .s_ready(enc_s_ready),
        .m_data(enc_m_data), .m_valid(enc_m_valid), .m_ready(enc_m_ready_r),
        .m_last(enc_m_last)
    );
    rs_syndrome u_synd (
        .clk(s_axi_aclk), .resetn(block_resetn),
        .s_data(push_data), .s_valid(synd_s_valid_r), .s_ready(synd_s_ready),
        .m_data(synd_m_data), .m_valid(synd_m_valid), .m_ready(synd_m_ready_r),
        .m_last(synd_m_last), .all_zero(synd_all_zero)
    );
    rs_chien_search u_chien (
        .clk(s_axi_aclk), .resetn(block_resetn),
        .s_data(push_data), .s_valid(chien_s_valid_r), .s_ready(chien_s_ready),
        .s_last(chien_s_last_r),
        .m_data(chien_m_data), .m_valid(chien_m_valid), .m_ready(chien_m_ready_r),
        .search_done(chien_search_done)
    );
    rs_forney u_forney (
        .clk(s_axi_aclk), .resetn(block_resetn),
        .cs_data(push_data), .cs_valid(forney_cs_valid_r), .cs_ready(forney_cs_ready),
        .c_last(forney_c_last_r), .cs_last(forney_cs_last_r),
        .d_data(push_data), .d_valid(forney_d_valid_r), .d_ready(forney_d_ready),
        .pos_data(forney_pos_data), .mag_data(forney_mag_data),
        .m_valid(forney_m_valid), .m_ready(forney_m_ready_r),
        .forney_fail(forney_fail)
    );
    rs_berlekamp_massey u_bm (
        .clk(s_axi_aclk), .resetn(block_resetn),
        .s_data(push_data), .s_valid(bm_s_valid_r), .s_ready(bm_s_ready),
        .m_data(bm_m_data), .m_valid(bm_m_valid), .m_ready(bm_m_ready_r),
        .m_last(bm_m_last), .degree(bm_degree), .done(bm_done)
    );

    wire       sel_s_ready  = (select==3'd0) ? enc_s_ready  : (select==3'd1) ? synd_s_ready  :
                               (select==3'd2) ? chien_s_ready : (select==3'd4) ? bm_s_ready :
                               forney_cs_ready;
    wire       sel_m_valid  = (select==3'd0) ? enc_m_valid  : (select==3'd1) ? synd_m_valid  :
                               (select==3'd2) ? chien_m_valid : (select==3'd4) ? bm_m_valid :
                               forney_m_valid;
    wire       sel_m_last   = (select==3'd0) ? enc_m_last   : (select==3'd1) ? synd_m_last   :
                               (select==3'd2) ? chien_done_latched : (select==3'd4) ? bm_m_last :
                               1'b0;
    wire [7:0] sel_m_data   = (select==3'd0) ? enc_m_data   : (select==3'd1) ? synd_m_data   :
                               (select==3'd4) ? bm_m_data : chien_m_data;
    wire       sel_all_zero = synd_all_zero;
    wire       sel_d_ready  = (select==3'd3) ? forney_d_ready : 1'b0;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            enc_s_valid_r <= 1'b0; synd_s_valid_r <= 1'b0; chien_s_valid_r <= 1'b0;
            enc_m_ready_r <= 1'b0; synd_m_ready_r <= 1'b0; chien_m_ready_r <= 1'b0;
            chien_s_last_r <= 1'b0; chien_done_latched <= 1'b0;
            forney_cs_valid_r <= 1'b0; forney_c_last_r <= 1'b0; forney_cs_last_r <= 1'b0;
            forney_d_valid_r <= 1'b0; forney_m_ready_r <= 1'b0;
            bm_s_valid_r <= 1'b0; bm_m_ready_r <= 1'b0; bm_done_latched <= 1'b0;
        end else begin
            enc_s_valid_r  <= 1'b0; synd_s_valid_r <= 1'b0; chien_s_valid_r <= 1'b0;
            enc_m_ready_r  <= 1'b0; synd_m_ready_r <= 1'b0; chien_m_ready_r <= 1'b0;
            chien_s_last_r <= 1'b0;
            forney_cs_valid_r <= 1'b0; forney_c_last_r <= 1'b0; forney_cs_last_r <= 1'b0;
            forney_d_valid_r  <= 1'b0; forney_m_ready_r <= 1'b0;
            bm_s_valid_r <= 1'b0; bm_m_ready_r <= 1'b0;
            if (!block_resetn) chien_done_latched <= 1'b0;
            else if (chien_search_done) chien_done_latched <= 1'b1;
            if (!block_resetn) bm_done_latched <= 1'b0;
            else if (bm_done) bm_done_latched <= 1'b1;

            if (push_valid_pulse || push_last_pulse) begin
                case (select)
                    3'd0: enc_s_valid_r   <= 1'b1;
                    3'd1: synd_s_valid_r  <= 1'b1;
                    3'd2: begin
                        chien_s_valid_r <= 1'b1;
                        chien_s_last_r  <= push_last_pulse;
                    end
                    3'd4: bm_s_valid_r <= 1'b1;
                    default: begin
                        forney_cs_valid_r <= 1'b1;
                        forney_c_last_r   <= push_last_pulse;
                    end
                endcase
            end
            if (push_cs_last_pulse) begin
                forney_cs_valid_r <= 1'b1;
                forney_cs_last_r  <= 1'b1;
            end
            if (push_degree_pulse) forney_d_valid_r <= 1'b1;

            if (pop_ready_pulse) begin
                case (select)
                    3'd0: enc_m_ready_r   <= 1'b1;
                    3'd1: synd_m_ready_r  <= 1'b1;
                    3'd4: bm_m_ready_r    <= 1'b1;
                    default: chien_m_ready_r <= 1'b1;
                endcase
            end
            if (pop_mag_ready_pulse) forney_m_ready_r <= 1'b1;
        end
    end

    reg aw_done, w_done;
    assign s_axi_awready = ~aw_done;
    assign s_axi_wready  = ~w_done;
    reg [5:0] awaddr_latched;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            aw_done <= 1'b0; w_done <= 1'b0;
            s_axi_bvalid <= 1'b0; s_axi_bresp <= 2'b00;
            select <= 3'd0; block_resetn <= 1'b0; do_reset_pulse <= 1'b0;
            push_data <= 8'h00; push_valid_pulse <= 1'b0; push_last_pulse <= 1'b0;
            push_cs_last_pulse <= 1'b0; push_degree_pulse <= 1'b0;
        end else begin
            push_valid_pulse <= 1'b0; push_last_pulse <= 1'b0;
            push_cs_last_pulse <= 1'b0; push_degree_pulse <= 1'b0;
            if (do_reset_pulse) begin
                block_resetn   <= 1'b1;
                do_reset_pulse <= 1'b0;
            end
            if (s_axi_awvalid && s_axi_awready) begin
                aw_done <= 1'b1; awaddr_latched <= s_axi_awaddr;
            end
            if (s_axi_wvalid && s_axi_wready) w_done <= 1'b1;
            if (aw_done && w_done && !s_axi_bvalid) begin
                case (awaddr_latched[5:2])
                    4'h1: begin // SELECT
                        select         <= s_axi_wdata[2:0];
                        block_resetn   <= 1'b0;
                        do_reset_pulse <= 1'b1;
                    end
                    4'h2: begin // PUSH
                        push_data        <= s_axi_wdata[7:0];
                        push_valid_pulse <= 1'b1;
                    end
                    4'h5: begin // PUSH_LAST
                        push_data       <= s_axi_wdata[7:0];
                        push_last_pulse <= 1'b1;
                    end
                    4'h6: begin // PUSH_CS_LAST
                        push_data          <= s_axi_wdata[7:0];
                        push_cs_last_pulse <= 1'b1;
                    end
                    4'h7: begin // PUSH_DEGREE
                        push_data         <= s_axi_wdata[7:0];
                        push_degree_pulse <= 1'b1;
                    end
                    default: ;
                endcase
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
            end
            if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
                aw_done <= 1'b0; w_done <= 1'b0;
            end
        end
    end

    reg ar_done;
    assign s_axi_arready = ~ar_done & ~s_axi_rvalid;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            ar_done <= 1'b0; s_axi_rvalid <= 1'b0;
            s_axi_rdata <= 32'd0; s_axi_rresp <= 2'b00;
            pop_ready_pulse <= 1'b0; pop_mag_ready_pulse <= 1'b0;
        end else begin
            pop_ready_pulse <= 1'b0; pop_mag_ready_pulse <= 1'b0;
            if (s_axi_arvalid && s_axi_arready) begin
                ar_done      <= 1'b1;
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                case (s_axi_araddr[5:2])
                    4'h0: s_axi_rdata <= MAGIC;
                    4'h1: s_axi_rdata <= {30'd0, select};
                    4'h3: s_axi_rdata <= {25'd0, bm_done_latched, forney_fail, sel_all_zero,
                                           sel_d_ready, sel_m_last, sel_m_valid, sel_s_ready};
                    4'h4: begin
                        s_axi_rdata     <= {24'd0, sel_m_data};
                        pop_ready_pulse <= 1'b1;
                    end
                    4'h8: s_axi_rdata <= {24'd0, forney_pos_data}; // POP_POS
                    4'h9: begin                                     // POP_MAG
                        s_axi_rdata         <= {24'd0, forney_mag_data};
                        pop_mag_ready_pulse <= 1'b1;
                    end
                    4'hA: s_axi_rdata <= {26'd0, bm_degree};        // DEGREE
                    default: s_axi_rdata <= 32'd0;
                endcase
            end
            if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
                ar_done      <= 1'b0;
            end
        end
    end
endmodule
