`timescale 1ns/1ps
module tb_axi_rs_test_harness;
    reg clk=0; always #5 clk=~clk;
    reg aresetn=0;

    reg [5:0]  awaddr; reg awvalid=0; wire awready;
    reg [31:0] wdata;  reg wvalid=0;  wire wready;
    wire [1:0] bresp;  wire bvalid;   reg bready=1;
    reg [5:0]  araddr; reg arvalid=0; wire arready;
    wire [31:0] rdata; wire rvalid;   reg rready=1;

    axi_rs_test_harness dut(
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

    localparam MAGIC=6'h00, SELECT=6'h04, PUSH=6'h08, STATUS=6'h0C, POP=6'h10, DEGREE=6'h14;

    integer i, errors;
    reg [7:0] data223 [0:222];
    reg [7:0] expected255 [0:254];

    initial begin
        errors=0;
        repeat(3) @(negedge clk); aresetn=1;

        axi_read(MAGIC);
        if (rd_result !== 32'h52535448) begin
            $display("MAGIC MISMATCH: got %08h", rd_result); errors=errors+1;
        end else $display("MAGIC OK: %08h", rd_result);

        // ── Exercise the encoder: push 223 bytes, pop 255 ──────────────
        axi_write(SELECT, 32'd0);
        for (i=0;i<223;i=i+1) data223[i] = i[7:0] ^ 8'hA5;
        // rs_encoder is a zero-depth passthrough: each accepted byte makes
        // the output valid immediately, and it will not accept the NEXT byte
        // until this one is popped. Push and pop interleaved, one at a time.
        for (i=0;i<223;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[0]) axi_read(STATUS); // wait s_ready
            axi_write(PUSH, {24'd0, data223[i]});
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS); // wait m_valid
            axi_read(POP);
            expected255[i] = rd_result[7:0];
            if (rd_result[7:0] !== data223[i]) begin
                $display("ENCODER PASSTHROUGH MISMATCH byte %0d: got %02h expected %02h",
                          i, rd_result[7:0], data223[i]);
                errors=errors+1;
            end
        end
        // Drain the 32 parity bytes the same way: nothing more to push, just
        // keep popping until all 255 codeword bytes have been read.
        for (i=223;i<255;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP);
            expected255[i] = rd_result[7:0];
        end
        $display("encoder exercised: 223 in, 255 out (passthrough checked)");

        // ── Exercise the syndrome calculator on the encoder's own output:
        //    a clean codeword must produce all-zero syndromes. ───────────
        axi_write(SELECT, 32'd1);
        for (i=0;i<255;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[0]) axi_read(STATUS);
            axi_write(PUSH, {24'd0, expected255[i]});
        end
        for (i=0;i<32;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP);
            if (rd_result[7:0] !== 8'h00) begin
                $display("SYNDROME NONZERO on clean codeword, byte %0d: %02h", i, rd_result[7:0]);
                errors=errors+1;
            end
        end
        axi_read(STATUS);
        if (!rd_result[4]) begin
            $display("all_zero flag not set on a clean codeword's syndromes"); errors=errors+1;
        end else $display("syndrome calculator: clean codeword -> all-zero syndromes, flag set");

        // ── Exercise Berlekamp-Massey on a corrupted codeword's syndromes:
        //    flip 3 bytes, recompute syndromes via the SAME harness, feed
        //    them to BM, expect degree==3. ────────────────────────────────
        expected255[5]  = expected255[5]  ^ 8'h11;
        expected255[100]= expected255[100]^ 8'h22;
        expected255[200]= expected255[200]^ 8'h33;
        axi_write(SELECT, 32'd1);
        for (i=0;i<255;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[0]) axi_read(STATUS);
            axi_write(PUSH, {24'd0, expected255[i]});
        end
        begin : synd_collect
            reg [7:0] synd [0:31];
            for (i=0;i<32;i=i+1) begin
                axi_read(STATUS);
                while (!rd_result[1]) axi_read(STATUS);
                axi_read(POP);
                synd[i] = rd_result[7:0];
            end
            axi_write(SELECT, 32'd2);
            for (i=0;i<32;i=i+1) begin
                axi_read(STATUS);
                while (!rd_result[0]) axi_read(STATUS);
                axi_write(PUSH, {24'd0, synd[i]});
            end
        end
        axi_read(STATUS);
        while (!rd_result[3]) axi_read(STATUS); // wait done
        axi_read(DEGREE);
        if (rd_result !== 32'd3) begin
            $display("BM DEGREE MISMATCH: got %0d expected 3", rd_result); errors=errors+1;
        end else $display("BM: 3 corrupted bytes -> degree=3, correct");
        axi_read(STATUS);
        while (!rd_result[1]) axi_read(STATUS);
        axi_read(POP);
        if (rd_result[7:0] !== 8'h01) begin
            $display("BM first locator coefficient wrong: got %02h expected 01", rd_result[7:0]);
            errors=errors+1;
        end

        if (errors==0) $display("PASS: AXI-Lite test harness exercises encoder, syndrome, BM correctly");
        else $fatal(1, "%0d errors", errors);
        $finish;
    end
    initial begin #2000000; $fatal(1,"timeout"); end
endmodule
