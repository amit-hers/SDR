`timescale 1ns/1ps
module tb_axi_rs_test_harness_hw;
    reg clk=0; always #5 clk=~clk;
    reg aresetn=0;

    reg [5:0]  awaddr; reg awvalid=0; wire awready;
    reg [31:0] wdata;  reg wvalid=0;  wire wready;
    wire [1:0] bresp;  wire bvalid;   reg bready=1;
    reg [5:0]  araddr; reg arvalid=0; wire arready;
    wire [31:0] rdata; wire rvalid;   reg rready=1;

    axi_rs_test_harness_hw dut(
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

    localparam MAGIC=6'h00, SELECT=6'h04, PUSH=6'h08, STATUS=6'h0C, POP=6'h10, PUSH_LAST=6'h14;
    localparam PUSH_CS_LAST=6'h18, PUSH_DEGREE=6'h1C, POP_POS=6'h20, POP_MAG=6'h24, DEGREE=6'h28;

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

        axi_write(SELECT, 32'd0);
        for (i=0;i<223;i=i+1) data223[i] = i[7:0] ^ 8'hA5;
        for (i=0;i<223;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[0]) axi_read(STATUS);
            axi_write(PUSH, {24'd0, data223[i]});
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP);
            expected255[i] = rd_result[7:0];
            if (rd_result[7:0] !== data223[i]) begin
                $display("ENCODER PASSTHROUGH MISMATCH byte %0d: got %02h expected %02h",
                          i, rd_result[7:0], data223[i]);
                errors=errors+1;
            end
        end
        for (i=223;i<255;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP);
            expected255[i] = rd_result[7:0];
        end
        $display("encoder exercised: 223 in, 255 out (passthrough checked)");

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

        // Corrupt one byte: syndromes must now be nonzero and all_zero clear.
        expected255[5] = expected255[5] ^ 8'h11;
        axi_write(SELECT, 32'd1);
        for (i=0;i<255;i=i+1) begin
            axi_read(STATUS);
            while (!rd_result[0]) axi_read(STATUS);
            axi_write(PUSH, {24'd0, expected255[i]});
        end
        begin : check_nonzero
            reg saw_nonzero;
            saw_nonzero = 0;
            for (i=0;i<32;i=i+1) begin
                axi_read(STATUS);
                while (!rd_result[1]) axi_read(STATUS);
                axi_read(POP);
                if (rd_result[7:0] !== 8'h00) saw_nonzero = 1;
            end
            if (!saw_nonzero) begin
                $display("EXPECTED nonzero syndromes on a corrupted codeword, got all zero");
                errors=errors+1;
            end
        end
        axi_read(STATUS);
        if (rd_result[4]) begin
            $display("all_zero flag WRONGLY set on a corrupted codeword"); errors=errors+1;
        end else $display("syndrome calculator: corrupted codeword -> nonzero syndromes, flag clear");

        // ── Chien search: known locator C=[0x01,0x0f,0x52] (a real BM
        //    output, degree 2), expected roots at degrees 154 and 249 --
        //    the exact test vector used to debug rs_forney.v earlier this
        //    session, so the expected values are independently known-good. ─
        axi_write(SELECT, 32'd2);
        axi_write(PUSH, 32'h01);
        axi_write(PUSH, 32'h0f);
        axi_write(PUSH_LAST, 32'h52);
        begin : chien_check
            integer got0, got1;
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP);
            got0 = rd_result[7:0];
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP);
            got1 = rd_result[7:0];
            if (got0 !== 154 || got1 !== 249) begin
                $display("CHIEN SEARCH MISMATCH: got degrees %0d,%0d expected 154,249", got0, got1);
                errors = errors + 1;
            end else $display("chien search: found degrees 154 and 249, correct");
            // A few more internal search cycles (250..254) still need to
            // elapse after the last root before search_done latches -- poll
            // rather than check once.
            got0 = 0;
            for (i = 0; i < 20 && !got0; i = i + 1) begin
                axi_read(STATUS);
                got0 = rd_result[2];
            end
            if (!got0) begin
                $display("chien search_done (latched) never set after both roots found");
                errors = errors + 1;
            end
        end

        // ── Forney: same known locator C=[0x01,0x0f,0x52] plus its real
        //    syndromes (both from the same encode+corrupt case used to debug
        //    rs_forney.v earlier this session), expect magnitudes 0x77 at
        //    degree 154 and 0x33 at degree 249 -- the exact injected error
        //    bytes, independently known-good. ────────────────────────────
        begin : forney_check
            reg [7:0] S [0:31];
            integer got_pos, got_mag, got_fail;
            S[0]=8'h44;S[1]=8'h1e;S[2]=8'hc6;S[3]=8'h04;S[4]=8'h86;S[5]=8'ha4;S[6]=8'h91;S[7]=8'h99;
            S[8]=8'h3d;S[9]=8'h85;S[10]=8'h96;S[11]=8'h74;S[12]=8'h1c;S[13]=8'h03;S[14]=8'ha7;S[15]=8'hf5;
            S[16]=8'h49;S[17]=8'h07;S[18]=8'hec;S[19]=8'h2e;S[20]=8'h81;S[21]=8'h15;S[22]=8'he3;S[23]=8'h96;
            S[24]=8'h2e;S[25]=8'h6d;S[26]=8'h98;S[27]=8'h5d;S[28]=8'hcd;S[29]=8'ha2;S[30]=8'hd6;S[31]=8'ha2;

            axi_write(SELECT, 32'd3);
            axi_write(PUSH, 32'h01);
            axi_write(PUSH, 32'h0f);
            axi_write(PUSH_LAST, 32'h52);
            for (i=0;i<31;i=i+1) axi_write(PUSH, {24'd0, S[i]});
            axi_write(PUSH_CS_LAST, {24'd0, S[31]});

            // request degree 154
            axi_read(STATUS);
            while (!rd_result[3]) axi_read(STATUS); // d_ready
            axi_write(PUSH_DEGREE, 32'd154);
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS); // m_valid
            axi_read(POP_POS); got_pos = rd_result[7:0];
            axi_read(POP_MAG); got_mag = rd_result[7:0];
            axi_read(STATUS); got_fail = rd_result[5];
            if (got_pos !== 154 || got_mag !== 8'h77 || got_fail) begin
                $display("FORNEY MISMATCH (req 154): pos=%0d mag=%02h fail=%b expected pos=154 mag=77 fail=0",
                         got_pos, got_mag, got_fail);
                errors = errors + 1;
            end else $display("forney: degree 154 -> magnitude 0x77, correct");

            // request degree 249
            axi_read(STATUS);
            while (!rd_result[3]) axi_read(STATUS);
            axi_write(PUSH_DEGREE, 32'd249);
            axi_read(STATUS);
            while (!rd_result[1]) axi_read(STATUS);
            axi_read(POP_POS); got_pos = rd_result[7:0];
            axi_read(POP_MAG); got_mag = rd_result[7:0];
            axi_read(STATUS); got_fail = rd_result[5];
            if (got_pos !== 249 || got_mag !== 8'h33 || got_fail) begin
                $display("FORNEY MISMATCH (req 249): pos=%0d mag=%02h fail=%b expected pos=249 mag=33 fail=0",
                         got_pos, got_mag, got_fail);
                errors = errors + 1;
            end else $display("forney: degree 249 -> magnitude 0x33, correct");
        end

        // ── Berlekamp-Massey: the SAME 32 syndromes used in forney_check
        //    above, from the real encode+corrupt case whose error-locator is
        //    independently known to be C=[0x01,0x0f,0x52] (degree 2) -- the
        //    exact vector Chien search and Forney already proved correct
        //    with, so this closes the loop: BM should derive that same
        //    locator directly from the syndromes, on real hardware. ────────
        begin : bm_check
            reg [7:0] S [0:31];
            reg [7:0] expected_C [0:2];
            integer got_degree, got_last;
            S[0]=8'h44;S[1]=8'h1e;S[2]=8'hc6;S[3]=8'h04;S[4]=8'h86;S[5]=8'ha4;S[6]=8'h91;S[7]=8'h99;
            S[8]=8'h3d;S[9]=8'h85;S[10]=8'h96;S[11]=8'h74;S[12]=8'h1c;S[13]=8'h03;S[14]=8'ha7;S[15]=8'hf5;
            S[16]=8'h49;S[17]=8'h07;S[18]=8'hec;S[19]=8'h2e;S[20]=8'h81;S[21]=8'h15;S[22]=8'he3;S[23]=8'h96;
            S[24]=8'h2e;S[25]=8'h6d;S[26]=8'h98;S[27]=8'h5d;S[28]=8'hcd;S[29]=8'ha2;S[30]=8'hd6;S[31]=8'ha2;
            expected_C[0]=8'h01; expected_C[1]=8'h0f; expected_C[2]=8'h52;

            axi_write(SELECT, 32'd4);
            for (i=0;i<32;i=i+1) begin
                axi_read(STATUS);
                while (!rd_result[0]) axi_read(STATUS); // s_ready
                axi_write(PUSH, {24'd0, S[i]});
            end

            got_degree = -1;
            for (i = 0; i < 400 && got_degree < 0; i = i + 1) begin
                axi_read(STATUS);
                if (rd_result[6]) got_degree = 1; // bm_done_latched
            end
            if (got_degree < 0) begin
                $display("BM done_latched never set after 32 syndromes pushed");
                errors = errors + 1;
            end else begin
                axi_read(DEGREE);
                if (rd_result[5:0] !== 6'd2) begin
                    $display("BM DEGREE MISMATCH: got %0d expected 2", rd_result[5:0]);
                    errors = errors + 1;
                end else $display("bm: degree correctly computed as 2");

                for (i = 0; i <= 2; i = i + 1) begin
                    axi_read(STATUS);
                    while (!rd_result[1]) axi_read(STATUS); // m_valid
                    got_last = rd_result[2];
                    axi_read(POP);
                    if (rd_result[7:0] !== expected_C[i]) begin
                        $display("BM COEFFICIENT MISMATCH byte %0d: got %02h expected %02h",
                                  i, rd_result[7:0], expected_C[i]);
                        errors = errors + 1;
                    end
                    if (i==2 && !got_last) begin
                        $display("BM m_last not set on final coefficient byte");
                        errors = errors + 1;
                    end
                    if (i<2 && got_last) begin
                        $display("BM m_last WRONGLY set early, on byte %0d", i);
                        errors = errors + 1;
                    end
                end
                if (errors==0) $display("bm: error-locator coefficients [01,0f,52] match, m_last correct");
            end
        end

        if (errors==0) $display("PASS: HW harness (encoder+syndrome+chien+forney+bm) exercised correctly");
        else $fatal(1, "%0d errors", errors);
        $finish;
    end
    initial begin #8000000; $fatal(1,"timeout"); end
endmodule
