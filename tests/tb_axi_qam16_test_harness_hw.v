`timescale 1ns/1ps
module tb_axi_qam16_test_harness_hw;
    reg clk=0; always #5 clk=~clk;
    reg aresetn=0;

    reg [5:0]  awaddr; reg awvalid=0; wire awready;
    reg [31:0] wdata;  reg wvalid=0;  wire wready;
    wire [1:0] bresp;  wire bvalid;   reg bready=1;
    reg [5:0]  araddr; reg arvalid=0; wire arready;
    wire [31:0] rdata; wire rvalid;   reg rready=1;

    axi_qam16_test_harness_hw dut(
        .s_axi_aclk(clk), .s_axi_aresetn(aresetn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'b0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(), .s_axi_rvalid(rvalid), .s_axi_rready(rready)
    );

    task axi_write(input [5:0] addr, input [31:0] data);
        begin
            @(negedge clk);
            awaddr<=addr; awvalid<=1; wdata<=data; wvalid<=1;
            @(posedge clk);
            while (!(awvalid&&awready)) @(posedge clk);
            @(negedge clk); awvalid<=0; wvalid<=0;
            while (!bvalid) @(posedge clk);
            @(negedge clk);
            while (bvalid) @(posedge clk);
        end
    endtask

    reg [31:0] rd_result;
    task axi_read(input [5:0] addr);
        begin
            @(negedge clk);
            araddr<=addr; arvalid<=1;
            @(posedge clk);
            while (!(arvalid&&arready)) @(posedge clk);
            @(negedge clk); arvalid<=0;
            while (!rvalid) @(posedge clk);
            rd_result = rdata;
            @(negedge clk);
            while (rvalid) @(posedge clk);
        end
    endtask

    localparam MAGIC=6'h00, PUSH_SYM=6'h04, MAP_STATUS=6'h08, POP_I=6'h0C, POP_Q=6'h10;
    localparam PUSH_I=6'h14, PUSH_Q=6'h18, DEMAP_STATUS=6'h1C, POP_SYM=6'h20, POP_ERROR=6'h24;

    integer i, errors;
    reg signed [15:0] level [0:3];

    initial begin
        errors=0;
        level[0]=-16'sd7773; level[1]=-16'sd2591; level[2]=16'sd2591; level[3]=16'sd7773;
        repeat(3) @(negedge clk); aresetn=1;

        axi_read(MAGIC);
        if (rd_result !== 32'h51313654) begin
            $display("MAGIC MISMATCH: got %08h", rd_result); errors=errors+1;
        end else $display("MAGIC OK: %08h", rd_result);

        // ── Mapper: all 16 symbols, check I/Q against the known Gray/level
        //    table (the same construction as qam16_mapper.v's own function,
        //    kept independent here rather than copy-pasted, by indexing into
        //    `level` with the Gray-to-binary conversion spelled out). ──────
        for (i=0;i<16;i=i+1) begin
            begin : map_check
                reg [1:0] gi, gq, bi, bq;
                reg signed [15:0] exp_i, exp_q;
                gi = i[3:2]; gq = i[1:0];
                bi = {gi[1], gi[1]^gi[0]};
                bq = {gq[1], gq[1]^gq[0]};
                exp_i = level[bi]; exp_q = level[bq];

                axi_read(MAP_STATUS);
                while (!rd_result[0]) axi_read(MAP_STATUS); // s_ready
                axi_write(PUSH_SYM, {28'd0, i[3:0]});
                axi_read(MAP_STATUS);
                while (!rd_result[1]) axi_read(MAP_STATUS); // m_valid
                axi_read(POP_I);
                if ($signed(rd_result[15:0]) !== exp_i) begin
                    $display("MAPPER I MISMATCH sym %0d: got %0d expected %0d", i, $signed(rd_result[15:0]), exp_i);
                    errors=errors+1;
                end
                axi_read(POP_Q);
                if ($signed(rd_result[15:0]) !== exp_q) begin
                    $display("MAPPER Q MISMATCH sym %0d: got %0d expected %0d", i, $signed(rd_result[15:0]), exp_q);
                    errors=errors+1;
                end
            end
        end
        if (errors==0) $display("mapper: all 16 symbols match expected Gray/level table");

        // ── Demapper: feed back each of the 16 constellation points exactly
        //    (zero error expected), then boundary/clipping cases. ─────────
        for (i=0;i<16;i=i+1) begin
            begin : demap_check
                reg [1:0] gi, gq, bi, bq;
                reg signed [15:0] send_i, send_q;
                gi = i[3:2]; gq = i[1:0];
                bi = {gi[1], gi[1]^gi[0]};
                bq = {gq[1], gq[1]^gq[0]};
                send_i = level[bi]; send_q = level[bq];

                axi_read(DEMAP_STATUS);
                while (!rd_result[0]) axi_read(DEMAP_STATUS); // s_ready
                axi_write(PUSH_I, {16'd0, send_i});
                axi_write(PUSH_Q, {16'd0, send_q});
                axi_read(DEMAP_STATUS);
                while (!rd_result[1]) axi_read(DEMAP_STATUS); // m_valid
                axi_read(POP_SYM);
                if (rd_result[3:0] !== i[3:0]) begin
                    $display("DEMAPPER SYMBOL MISMATCH at point %0d: got %0d", i, rd_result[3:0]);
                    errors=errors+1;
                end
                axi_read(POP_ERROR);
                if (rd_result !== 32'd0) begin
                    $display("DEMAPPER ERROR NONZERO on exact constellation point %0d: %0d", i, rd_result);
                    errors=errors+1;
                end
            end
        end
        if (errors==0) $display("demapper: all 16 exact constellation points decode correctly, zero error");

        // Clipping: full-scale rails on both axes.
        axi_read(DEMAP_STATUS);
        while (!rd_result[0]) axi_read(DEMAP_STATUS);
        axi_write(PUSH_I, 32'h00007FFF);
        axi_write(PUSH_Q, 32'h00008000);
        axi_read(DEMAP_STATUS);
        while (!rd_result[1]) axi_read(DEMAP_STATUS);
        if (!rd_result[2]) begin
            $display("CLIPPING FLAG not set on full-scale I/Q"); errors=errors+1;
        end else $display("demapper: clipping flag correctly set on full-scale I/Q");
        axi_read(POP_SYM);
        axi_read(POP_ERROR);

        if (errors==0) $display("PASS: HW 16-QAM harness (mapper+demapper) exercised correctly");
        else $fatal(1, "%0d errors", errors);
        $finish;
    end
    initial begin #2000000; $fatal(1,"timeout"); end
endmodule
