// Hardware test harness for the INTEGRATED RS(255,223) decoder core
// (fpga/rtl/rs_decoder.v) -- the full syndrome->BM->Chien->Forney chain
// running autonomously in hardware, not the individual blocks the existing
// axi_rs_test_harness_hw.v already proved separately. Same register
// conventions as that harness (MAGIC/STATUS/peek-then-consume POP), the
// same pattern already proven safe on real silicon for every block family
// tested this session.
//
// Unlike the per-block harness, there is no SELECT register here -- only
// one core, with a much simpler protocol than any individual block's
// multi-phase interface: push exactly 255 received bytes (no "last" marker
// needed, rs_decoder.v fixed-counts internally, same convention as
// rs_berlekamp_massey.v's 32-syndrome input), then pop exactly 223
// corrected/passthrough bytes, reading STATUS.fail and ERROR_COUNT once
// STATUS.m_last is seen on the final popped byte.
//
// Register map (byte offsets, 32-bit registers):
//   0x00 MAGIC        RO  0x52534443 ("RSDC")
//   0x04 PUSH         WO  wdata[7:0] -> s_data, s_valid pulsed for one
//                         cycle. Check STATUS.s_ready (bit0) first -- a
//                         push issued while not ready is lost.
//   0x08 STATUS       RO  bit0 s_ready, bit1 m_valid, bit2 m_last,
//                         bit3 fail (valid alongside m_last, stays stable
//                         through the whole output phase -- see
//                         rs_decoder.v's own header for why it's gated on
//                         actual consumption, not just reaching the last
//                         index)
//   0x0C POP          RO  returns m_data; this SAME read also pulses
//                         m_ready for one cycle (fetch-and-acknowledge in
//                         one transaction).
//   0x10 ERROR_COUNT  RO  bytes corrected this codeword (0 if clean or
//                         failed); valid alongside STATUS.m_last, same
//                         stability window as STATUS.fail.
//
// IMPORTANT ordering for the LAST byte of a codeword: read STATUS and
// ERROR_COUNT BEFORE issuing the POP that consumes that final byte, not
// after. Popping it is exactly what lets rs_decoder.v accept the NEXT
// codeword, which immediately starts clearing fail/error_count every cycle
// thereafter -- by the time a SEPARATE following AXI-Lite read transaction
// completes (several clock cycles of protocol overhead later), they have
// already been cleared back to 0. Caught by this harness's own testbench
// getting it backwards on the first attempt.
module axi_rs_decoder_test_harness_hw (
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
    localparam [31:0] MAGIC = 32'h52534443;

    reg  [7:0] push_data;
    reg        push_pulse;
    wire       dec_s_ready;
    wire [7:0] dec_m_data;
    wire       dec_m_valid, dec_m_last, dec_fail;
    wire [5:0] dec_error_count;

    reg        dec_s_valid_r, dec_m_ready_r;

    rs_decoder u_dec (
        .clk(s_axi_aclk), .resetn(s_axi_aresetn),
        .s_data(push_data), .s_valid(dec_s_valid_r), .s_ready(dec_s_ready),
        .m_data(dec_m_data), .m_valid(dec_m_valid), .m_ready(dec_m_ready_r), .m_last(dec_m_last),
        .fail(dec_fail), .error_count(dec_error_count)
    );

    reg pop_pulse;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            dec_s_valid_r <= 1'b0; dec_m_ready_r <= 1'b0;
        end else begin
            dec_s_valid_r <= 1'b0; dec_m_ready_r <= 1'b0;
            if (push_pulse) dec_s_valid_r <= 1'b1;
            if (pop_pulse)  dec_m_ready_r <= 1'b1;
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
            push_data <= 8'h00; push_pulse <= 1'b0;
        end else begin
            push_pulse <= 1'b0;
            if (s_axi_awvalid && s_axi_awready) begin
                aw_done <= 1'b1; awaddr_latched <= s_axi_awaddr;
            end
            if (s_axi_wvalid && s_axi_wready) w_done <= 1'b1;
            if (aw_done && w_done && !s_axi_bvalid) begin
                case (awaddr_latched[5:2])
                    4'h1: begin // PUSH
                        push_data  <= s_axi_wdata[7:0];
                        push_pulse <= 1'b1;
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
            pop_pulse <= 1'b0;
        end else begin
            pop_pulse <= 1'b0;
            if (s_axi_arvalid && s_axi_arready) begin
                ar_done      <= 1'b1;
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                case (s_axi_araddr[5:2])
                    4'h0: s_axi_rdata <= MAGIC;
                    4'h2: s_axi_rdata <= {28'd0, dec_fail, dec_m_last, dec_m_valid, dec_s_ready}; // STATUS
                    4'h3: begin                                                                    // POP
                        s_axi_rdata <= {24'd0, dec_m_data};
                        pop_pulse   <= 1'b1;
                    end
                    4'h4: s_axi_rdata <= {26'd0, dec_error_count};                                  // ERROR_COUNT
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
