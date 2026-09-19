`timescale 1ns/1ps

module tb_axi_version_id;
    reg clk = 0;
    reg resetn = 0;
    reg [5:0] araddr = 0;
    reg arvalid = 0;
    wire arready;
    wire [31:0] rdata;
    wire [1:0] rresp;
    wire rvalid;
    reg rready = 0;

    always #5 clk = ~clk;

    axi_version_id #(.RX_PKT_BYTES(32'd4096)) dut (
        .s_axi_aclk(clk), .s_axi_aresetn(resetn),
        .s_axi_awaddr(0), .s_axi_awprot(0), .s_axi_awvalid(0), .s_axi_awready(),
        .s_axi_wdata(0), .s_axi_wstrb(0), .s_axi_wvalid(0), .s_axi_wready(),
        .s_axi_bresp(), .s_axi_bvalid(), .s_axi_bready(0),
        .s_axi_araddr(araddr), .s_axi_arprot(0), .s_axi_arvalid(arvalid),
        .s_axi_arready(arready), .s_axi_rdata(rdata), .s_axi_rresp(rresp),
        .s_axi_rvalid(rvalid), .s_axi_rready(rready)
    );

    task read_expect;
        input [5:0] address;
        input [31:0] expected;
        begin
            @(negedge clk); araddr = address; arvalid = 1;
            @(negedge clk); arvalid = 0;
            wait (rvalid);
            if (rresp !== 0 || rdata !== expected) begin
                $display("FAIL address=%h got=%h expected=%h", address, rdata, expected);
                $fatal(1);
            end
            @(negedge clk); rready = 1;
            @(negedge clk); rready = 0;
        end
    endtask

    initial begin
        repeat (2) @(negedge clk);
        resetn = 1;
        read_expect(6'h00, 32'h5344524C);
        read_expect(6'h18, 32'd4096);
        read_expect(6'h1c, 32'd0);
        $display("PASS axi_version_id RX_PKT_BYTES");
        $finish;
    end
endmodule
