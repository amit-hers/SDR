// axi_version_id -- read-only AXI4-Lite identity block for the modem PL.
//
// Exists so software can REFUSE TO RUN against an FPGA it does not understand.
// Every register in this design is addressed by a hand-maintained offset, and
// the offsets have moved before: adding one scalar to the HLS demodulator once
// pushed bpsk_mode from 0x18 to 0x24 and silently broke every script writing the
// old address. Nothing about a stale bitstream announces itself -- the reads
// still succeed, they just mean something else -- so the only safe protocol is
// for the FPGA to state its own contract and for software to check it.
//
// Deliberately a separate peripheral rather than four more HLS scalars. HLS
// places s_axilite ports on an 8-byte grid of its own choosing, so it cannot
// honour the 0x00/0x04/0x08/0x0C layout, and the identity has to be readable
// even when the modem cores are held in reset or stalled -- which is exactly
// when a version mismatch is most likely to be the cause.
//
// All registers are READ-ONLY. Writes are accepted and discarded so that a
// careless write cannot brick the identity a recovery tool depends on.
//
//   0x00  MAGIC                 constant, proves this is our bitstream at all
//   0x04  FPGA_VERSION          0x00MMmmpp -- major, minor, patch
//   0x08  FPGA_ABI_VERSION      bumped when the register map changes meaning
//   0x0C  REGISTER_MAP_VERSION  bumped when a register moves or is added
//   0x10  BUILD_EPOCH           UTC seconds, ties the PL to a manifest
//   0x14  GIT_SHA              first 32 bits of the source commit
module axi_version_id #(
    parameter [31:0] MAGIC        = 32'h5344524C,  // "SDRL"
    parameter [31:0] FPGA_VERSION = 32'h00010300,  // 1.3.0
    parameter [31:0] FPGA_ABI     = 32'd3,
    parameter [31:0] REGMAP_VER   = 32'd3,
    parameter [31:0] BUILD_EPOCH  = 32'd0,
    parameter [31:0] GIT_SHA      = 32'd0
) (
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
    // Writes: always accept, never store. A write channel that never asserts
    // ready would hang the interconnect for any master that probes it.
    reg aw_done, w_done;
    assign s_axi_awready = ~aw_done;
    assign s_axi_wready  = ~w_done;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            aw_done <= 1'b0; w_done <= 1'b0;
            s_axi_bvalid <= 1'b0; s_axi_bresp <= 2'b00;
        end else begin
            if (s_axi_awvalid && s_axi_awready) aw_done <= 1'b1;
            if (s_axi_wvalid  && s_axi_wready ) w_done  <= 1'b1;
            if (aw_done && w_done && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;          // OKAY: discarded, not refused
            end
            if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
                aw_done <= 1'b0; w_done <= 1'b0;
            end
        end
    end

    // Reads: one register per word, byte address decoded on bits [5:2].
    reg ar_done;
    assign s_axi_arready = ~ar_done & ~s_axi_rvalid;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            ar_done <= 1'b0; s_axi_rvalid <= 1'b0;
            s_axi_rdata <= 32'd0; s_axi_rresp <= 2'b00;
        end else begin
            if (s_axi_arvalid && s_axi_arready) begin
                ar_done      <= 1'b1;
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                case (s_axi_araddr[5:2])
                    4'h0:    s_axi_rdata <= MAGIC;
                    4'h1:    s_axi_rdata <= FPGA_VERSION;
                    4'h2:    s_axi_rdata <= FPGA_ABI;
                    4'h3:    s_axi_rdata <= REGMAP_VER;
                    4'h4:    s_axi_rdata <= BUILD_EPOCH;
                    4'h5:    s_axi_rdata <= GIT_SHA;
                    // Unmapped words read zero rather than erroring: a probe
                    // walking the window must not wedge the interconnect.
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
