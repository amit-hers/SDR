// Hardware test harness for the 64-QAM mapper AND demapper
// (fpga/rtl/qam64_mapper.v, qam64_demapper.v) -- identical structure to
// axi_qam16_test_harness_hw.v.
//
// The demapper was ORIGINALLY dropped from this harness: OOC synthesis
// showed its single-stage version did not meet this project's real 100 MHz
// AXI-Lite clock (WNS -0.396 ns, 32 failing endpoints) -- its 8-way index()
// comparator was deeper than qam16_demapper.v's 4-way one, eating the
// margin that an otherwise near-identical squared-error DSP48E1 multiply
// path has. Fixed 2026-10-02 by pipelining qam64_demapper.v into two stages
// (index/level/diff registered separately from the square-and-sum), the
// same class of fix as rs_berlekamp_massey.v's pipelining -- see
// qam64_demapper.v's own header for the full story. Standalone OOC now
// measures WNS +4.478 ns at 100 MHz (up from -0.396 ns), so the demapper is
// back in this harness.
//
// Both blocks are exposed independently (not wired to each other) so each
// can be driven with known-good vectors and checked exactly, rather than
// only as a closed loop where a wrong mapper and a compensating wrong
// demapper could both look "correct."
//
// Register map (byte offsets, 32-bit registers):
//   0x00 MAGIC        RO  0x51363454 ("Q64T")
//   0x04 PUSH_SYM     WO  wdata[5:0] -> mapper s_data, s_valid pulsed for
//                         one cycle. Check MAP_STATUS.s_ready (bit0) first.
//   0x08 MAP_STATUS   RO  bit0 mapper s_ready, bit1 mapper m_valid
//   0x0C POP_I        RO  peek mapper m_i (signed 16-bit, sign-extended to
//                         32 bits). Read-only peek -- read BEFORE POP_Q.
//   0x10 POP_Q        RO  returns mapper m_q (signed, sign-extended); this
//                         SAME read pulses the mapper's own m_ready.
//   0x14 PUSH_I       WO  wdata[15:0] (signed) -> latches demapper s_i.
//   0x18 PUSH_Q       WO  wdata[15:0] (signed) -> latches demapper s_q AND
//                         pulses s_valid for one cycle (submits the pair
//                         latched via PUSH_I together with this Q value).
//                         Check DEMAP_STATUS.s_ready (bit0) first.
//   0x1C DEMAP_STATUS RO  bit0 demapper s_ready, bit1 demapper m_valid,
//                         bit2 demapper m_clipped
//   0x20 POP_SYM      RO  peek demapper m_data[5:0]. Read BEFORE POP_ERROR.
//   0x24 POP_ERROR    RO  returns demapper m_error[31:0] (squared
//                         constellation error, Q4.26); this SAME read
//                         pulses the demapper's own m_ready. The demapper
//                         is now a 2-cycle-latency pipeline internally, but
//                         that is invisible at this register interface --
//                         just poll DEMAP_STATUS.m_valid as usual.
module axi_qam64_test_harness_hw (
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
    localparam [31:0] MAGIC = 32'h51363454;

    reg        resetn;
    reg [5:0]  push_sym_data;
    reg        push_sym_pulse;
    reg signed [15:0] push_i_reg;
    reg signed [15:0] push_q_reg;
    reg        push_q_pulse;

    wire        map_s_ready, map_m_valid;
    wire signed [15:0] map_m_i, map_m_q;
    reg         map_s_valid_r, map_m_ready_r;

    wire        demap_s_ready, demap_m_valid, demap_m_clipped;
    wire [5:0]  demap_m_data;
    wire [31:0] demap_m_error;
    reg         demap_s_valid_r, demap_m_ready_r;

    qam64_mapper u_mapper (
        .clk(s_axi_aclk), .resetn(resetn),
        .s_data(push_sym_data), .s_valid(map_s_valid_r), .s_ready(map_s_ready),
        .m_i(map_m_i), .m_q(map_m_q), .m_valid(map_m_valid), .m_ready(map_m_ready_r)
    );
    qam64_demapper u_demapper (
        .clk(s_axi_aclk), .resetn(resetn),
        .s_i(push_i_reg), .s_q(push_q_reg), .s_valid(demap_s_valid_r), .s_ready(demap_s_ready),
        .m_data(demap_m_data), .m_error(demap_m_error), .m_clipped(demap_m_clipped),
        .m_valid(demap_m_valid), .m_ready(demap_m_ready_r)
    );

    reg pop_q_pulse;     // -> mapper m_ready
    reg pop_error_pulse; // -> demapper m_ready

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            resetn <= 1'b0;
            map_s_valid_r <= 1'b0; map_m_ready_r <= 1'b0;
            demap_s_valid_r <= 1'b0; demap_m_ready_r <= 1'b0;
        end else begin
            resetn <= 1'b1;
            map_s_valid_r <= 1'b0; map_m_ready_r <= 1'b0;
            demap_s_valid_r <= 1'b0; demap_m_ready_r <= 1'b0;
            if (push_sym_pulse) map_s_valid_r <= 1'b1;
            if (push_q_pulse)   demap_s_valid_r <= 1'b1;
            if (pop_q_pulse)    map_m_ready_r <= 1'b1;
            if (pop_error_pulse) demap_m_ready_r <= 1'b1;
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
            push_sym_data <= 6'h0; push_sym_pulse <= 1'b0;
            push_i_reg <= 16'sd0; push_q_reg <= 16'sd0; push_q_pulse <= 1'b0;
        end else begin
            push_sym_pulse <= 1'b0; push_q_pulse <= 1'b0;
            if (s_axi_awvalid && s_axi_awready) begin
                aw_done <= 1'b1; awaddr_latched <= s_axi_awaddr;
            end
            if (s_axi_wvalid && s_axi_wready) w_done <= 1'b1;
            if (aw_done && w_done && !s_axi_bvalid) begin
                case (awaddr_latched[5:2])
                    4'h1: begin // PUSH_SYM
                        push_sym_data  <= s_axi_wdata[5:0];
                        push_sym_pulse <= 1'b1;
                    end
                    4'h5: push_i_reg <= s_axi_wdata[15:0]; // PUSH_I
                    4'h6: begin // PUSH_Q
                        push_q_reg  <= s_axi_wdata[15:0];
                        push_q_pulse <= 1'b1;
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
            pop_q_pulse <= 1'b0; pop_error_pulse <= 1'b0;
        end else begin
            pop_q_pulse <= 1'b0; pop_error_pulse <= 1'b0;
            if (s_axi_arvalid && s_axi_arready) begin
                ar_done      <= 1'b1;
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                case (s_axi_araddr[5:2])
                    4'h0: s_axi_rdata <= MAGIC;
                    4'h2: s_axi_rdata <= {30'd0, map_m_valid, map_s_ready};       // MAP_STATUS
                    4'h3: s_axi_rdata <= {{16{map_m_i[15]}}, map_m_i};           // POP_I (peek)
                    4'h4: begin                                                  // POP_Q (consume)
                        s_axi_rdata  <= {{16{map_m_q[15]}}, map_m_q};
                        pop_q_pulse  <= 1'b1;
                    end
                    4'h7: s_axi_rdata <= {29'd0, demap_m_clipped, demap_m_valid, demap_s_ready}; // DEMAP_STATUS
                    4'h8: s_axi_rdata <= {26'd0, demap_m_data};                  // POP_SYM (peek)
                    4'h9: begin                                                  // POP_ERROR (consume)
                        s_axi_rdata     <= demap_m_error;
                        pop_error_pulse <= 1'b1;
                    end
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
