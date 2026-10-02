`timescale 1ns/1ps
module tb_axi_rs_decoder_test_harness_hw;
    localparam integer N_BLOCKS = 6;
    localparam integer K = 223;
    localparam integer N = 255;

    reg clk=0; always #5 clk=~clk;
    reg aresetn=0;

    reg [5:0]  awaddr; reg awvalid=0; wire awready;
    reg [31:0] wdata;  reg wvalid=0;  wire wready;
    wire [1:0] bresp;  wire bvalid;   reg bready=1;
    reg [5:0]  araddr; reg arvalid=0; wire arready;
    wire [31:0] rdata; wire rvalid;   reg rready=1;

    axi_rs_decoder_test_harness_hw dut(
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

    localparam MAGIC=6'h00, PUSH=6'h04, STATUS=6'h08, POP=6'h0C, ERROR_COUNT=6'h10;

    reg [7:0] recv_mem   [0:N*N_BLOCKS-1];
    reg [7:0] exp_mem    [0:K*N_BLOCKS-1];
    reg [7:0] fail_mem   [0:N_BLOCKS-1];
    reg [7:0] errcnt_mem [0:N_BLOCKS-1];
    reg [255:0] f_recv, f_exp, f_fail, f_errcnt;
    integer have_vectors;

    integer blk, i, errors;
    integer recv_base, exp_base;

    initial begin
        have_vectors=$value$plusargs("recv=%s",f_recv); if(!have_vectors) $fatal(1,"need +recv=");
        have_vectors=$value$plusargs("exp=%s",f_exp); if(!have_vectors) $fatal(1,"need +exp=");
        have_vectors=$value$plusargs("fail=%s",f_fail); if(!have_vectors) $fatal(1,"need +fail=");
        have_vectors=$value$plusargs("errcnt=%s",f_errcnt); if(!have_vectors) $fatal(1,"need +errcnt=");
        $readmemh(f_recv, recv_mem);
        $readmemh(f_exp, exp_mem);
        $readmemh(f_fail, fail_mem);
        $readmemh(f_errcnt, errcnt_mem);

        errors = 0;
        repeat(3) @(negedge clk); aresetn=1;

        axi_read(MAGIC);
        if (rd_result !== 32'h52534443) begin
            $display("MAGIC MISMATCH: got %08h", rd_result); errors=errors+1;
        end else $display("MAGIC OK: %08h", rd_result);

        for (blk = 0; blk < N_BLOCKS; blk = blk + 1) begin
            recv_base = blk * N;
            exp_base  = blk * K;

            for (i = 0; i < N; i = i + 1) begin
                axi_read(STATUS);
                while (!rd_result[0]) axi_read(STATUS); // s_ready
                axi_write(PUSH, {24'd0, recv_mem[recv_base + i]});
            end

            for (i = 0; i < K; i = i + 1) begin : pop_loop
                reg status_m_last, status_fail;
                axi_read(STATUS);
                while (!rd_result[1]) axi_read(STATUS); // m_valid
                // Capture m_last/fail from THIS SAME status word (the one
                // that confirmed m_valid for the byte about to be popped),
                // and read ERROR_COUNT here too if this is the last byte --
                // ALL before popping. Popping the final byte is exactly
                // what lets rs_decoder.v's s_ready return for the NEXT
                // codeword, which (by design, see rs_decoder.v's own
                // header) immediately starts clearing fail/error_count
                // every cycle thereafter -- reading them AFTER that pop,
                // as a separate later AXI transaction, raced the clear and
                // always lost under real AXI-Lite's multi-cycle-per-
                // transaction overhead, every single time.
                status_m_last = rd_result[2];
                status_fail   = rd_result[3];
                if (i == K-1) begin
                    if (!status_m_last) begin
                        $display("block %0d: STATUS.m_last not set on final byte", blk);
                        errors = errors + 1;
                    end
                    if (status_fail !== fail_mem[blk][0]) begin
                        $display("block %0d: fail=%b expected %b", blk, status_fail, fail_mem[blk][0]);
                        errors = errors + 1;
                    end
                    axi_read(ERROR_COUNT);
                    if (rd_result[5:0] !== errcnt_mem[blk][5:0]) begin
                        $display("block %0d: error_count=%0d expected %0d",
                                  blk, rd_result[5:0], errcnt_mem[blk]);
                        errors = errors + 1;
                    end
                end
                axi_read(POP);
                if (rd_result[7:0] !== exp_mem[exp_base + i]) begin
                    $display("block %0d byte %0d MISMATCH: got %02h expected %02h",
                              blk, i, rd_result[7:0], exp_mem[exp_base + i]);
                    errors = errors + 1;
                end
            end
        end

        if (errors==0) $display("PASS: HW decoder-core harness exercised correctly (%0d blocks, clean/corrected/uncorrectable mix)", N_BLOCKS);
        else $fatal(1, "%0d errors", errors);
        $finish;
    end
    initial begin #40000000; $fatal(1,"timeout"); end
endmodule
