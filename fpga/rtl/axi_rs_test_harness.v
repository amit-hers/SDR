// Minimal AXI4-Lite test harness around the RS(255,223) FPGA blocks built
// this session: rs_encoder, rs_syndrome, rs_berlekamp_massey. Exists ONLY to
// let the ARM exercise these blocks on real silicon via register polling --
// it is NOT part of any production signal path (no connection to the AD9361,
// the QPSK modem, or any DMA). Separate bitstream variant, separate from the
// deployed/qualified Phase 8 candidate.
//
// All three sub-blocks share the identical byte-stream handshake
// (s_data/s_valid/s_ready in, m_data/m_valid/m_ready/m_last out), which is
// what makes one small wrapper enough for all three rather than one each.
//
// Register map (byte offsets, 32-bit registers):
//   0x00 MAGIC    RO  0x52535448 ("RSTH") -- confirms this is the harness image
//   0x04 SELECT   RW  0=encoder 1=syndrome 2=berlekamp_massey. Any write also
//                     pulses that block's own reset, for a clean run.
//   0x08 PUSH     WO  wdata[7:0] -> selected block's s_data, s_valid pulsed
//                     for one cycle on the FOLLOWING clock. Software must
//                     check STATUS.s_ready is set before writing this --
//                     there is no backpressure feedback into the AXI write
//                     response, a push issued while not ready is simply lost.
//   0x0C STATUS   RO  bit0 s_ready, bit1 m_valid, bit2 m_last,
//                     bit3 done (BM only, else 0), bit4 all_zero (syndrome
//                     only, else 0)
//   0x10 POP      RO  returns m_data; this SAME read also pulses m_ready for
//                     one cycle, so reading POP both fetches and acknowledges
//                     the current output byte (do not read it twice expecting
//                     the same byte back).
//   0x14 DEGREE   RO  rs_berlekamp_massey's degree output, zero-extended;
//                     meaningless (reads 0) for the other two blocks.
module axi_rs_test_harness (
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

    // ── Per-block reset, pulsed on any SELECT write ─────────────────────
    reg [1:0] select;
    reg       block_resetn;
    reg       do_reset_pulse;

    // ── Shared push/pop signalling, muxed to whichever block is selected ─
    reg  [7:0] push_data;
    reg        push_valid_pulse;

    wire [7:0] enc_m_data,   synd_m_data,   bm_m_data;
    wire       enc_m_valid,  synd_m_valid,  bm_m_valid;
    wire       enc_m_last,   synd_m_last,   bm_m_last;
    wire       enc_s_ready,  synd_s_ready,  bm_s_ready;
    wire       synd_all_zero;
    wire       bm_done;
    // bm_done is a ONE-CYCLE pulse in rs_berlekamp_massey -- a software
    // poll loop over AXI-Lite reads (each taking several clock cycles) has
    // no guarantee of ever sampling STATUS on that exact cycle. Latch it
    // here so it stays set until the block is reset (any SELECT write).
    reg bm_done_latched;
    wire [5:0] bm_degree;

    reg        enc_m_ready_r, synd_m_ready_r, bm_m_ready_r;
    reg        enc_s_valid_r, synd_s_valid_r, bm_s_valid_r;
    reg        pop_ready_pulse;

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
    rs_berlekamp_massey u_bm (
        .clk(s_axi_aclk), .resetn(block_resetn),
        .s_data(push_data), .s_valid(bm_s_valid_r), .s_ready(bm_s_ready),
        .m_data(bm_m_data), .m_valid(bm_m_valid), .m_ready(bm_m_ready_r),
        .m_last(bm_m_last), .degree(bm_degree), .done(bm_done)
    );

    wire       sel_s_ready  = (select==2'd0) ? enc_s_ready  : (select==2'd1) ? synd_s_ready  : bm_s_ready;
    wire       sel_m_valid  = (select==2'd0) ? enc_m_valid  : (select==2'd1) ? synd_m_valid  : bm_m_valid;
    wire       sel_m_last   = (select==2'd0) ? enc_m_last   : (select==2'd1) ? synd_m_last   : bm_m_last;
    wire [7:0] sel_m_data   = (select==2'd0) ? enc_m_data   : (select==2'd1) ? synd_m_data   : bm_m_data;
    wire       sel_all_zero = synd_all_zero;
    wire       sel_done     = bm_done_latched;

    // One-cycle pulses: valid on push, ready on pop. Latched registers
    // below the sub-blocks' own clock, cleared the cycle after.
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            enc_s_valid_r <= 1'b0; synd_s_valid_r <= 1'b0; bm_s_valid_r <= 1'b0;
            enc_m_ready_r <= 1'b0; synd_m_ready_r <= 1'b0; bm_m_ready_r <= 1'b0;
            bm_done_latched <= 1'b0;
        end else begin
            enc_s_valid_r  <= 1'b0; synd_s_valid_r <= 1'b0; bm_s_valid_r <= 1'b0;
            enc_m_ready_r  <= 1'b0; synd_m_ready_r <= 1'b0; bm_m_ready_r <= 1'b0;
            if (!block_resetn) bm_done_latched <= 1'b0;
            else if (bm_done)  bm_done_latched <= 1'b1;
            if (push_valid_pulse) begin
                case (select)
                    2'd0: enc_s_valid_r  <= 1'b1;
                    2'd1: synd_s_valid_r <= 1'b1;
                    default: bm_s_valid_r <= 1'b1;
                endcase
            end
            if (pop_ready_pulse) begin
                case (select)
                    2'd0: enc_m_ready_r  <= 1'b1;
                    2'd1: synd_m_ready_r <= 1'b1;
                    default: bm_m_ready_r <= 1'b1;
                endcase
            end
        end
    end

    // ── AXI-Lite write channel ──────────────────────────────────────────
    reg aw_done, w_done;
    assign s_axi_awready = ~aw_done;
    assign s_axi_wready  = ~w_done;
    reg [5:0] awaddr_latched;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            aw_done <= 1'b0; w_done <= 1'b0;
            s_axi_bvalid <= 1'b0; s_axi_bresp <= 2'b00;
            select <= 2'd0; block_resetn <= 1'b0; do_reset_pulse <= 1'b0;
            push_data <= 8'h00; push_valid_pulse <= 1'b0;
        end else begin
            push_valid_pulse <= 1'b0;
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
                        select         <= s_axi_wdata[1:0];
                        block_resetn   <= 1'b0;   // pulsed low this cycle,
                        do_reset_pulse <= 1'b1;   // released next cycle
                    end
                    4'h2: begin // PUSH
                        push_data        <= s_axi_wdata[7:0];
                        push_valid_pulse <= 1'b1;
                    end
                    default: ; // MAGIC/STATUS/POP/DEGREE are read-only
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

    // ── AXI-Lite read channel ────────────────────────────────────────────
    reg ar_done;
    assign s_axi_arready = ~ar_done & ~s_axi_rvalid;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            ar_done <= 1'b0; s_axi_rvalid <= 1'b0;
            s_axi_rdata <= 32'd0; s_axi_rresp <= 2'b00;
            pop_ready_pulse <= 1'b0;
        end else begin
            pop_ready_pulse <= 1'b0;
            if (s_axi_arvalid && s_axi_arready) begin
                ar_done      <= 1'b1;
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                case (s_axi_araddr[5:2])
                    4'h0: s_axi_rdata <= MAGIC;
                    4'h1: s_axi_rdata <= {30'd0, select};
                    4'h3: s_axi_rdata <= {27'd0, sel_all_zero, sel_done,
                                           sel_m_last, sel_m_valid, sel_s_ready};
                    4'h4: begin
                        s_axi_rdata     <= {24'd0, sel_m_data};
                        pop_ready_pulse <= 1'b1;
                    end
                    4'h5: s_axi_rdata <= {26'd0, bm_degree};
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
