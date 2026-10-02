`timescale 1ns/1ps
module tb_rs_decoder;
    localparam integer N_BLOCKS = 12;
    localparam integer K = 223;
    localparam integer N = 255;

    reg clk=0; always #5 clk=~clk;
    reg resetn=0;
    reg [7:0] s_data=0; reg s_valid=0;
    wire s_ready;
    wire [7:0] m_data; wire m_valid; reg m_ready=0; wire m_last;
    wire fail; wire [5:0] error_count;

    rs_decoder dut(clk, resetn, s_data, s_valid, s_ready,
                   m_data, m_valid, m_ready, m_last, fail, error_count);

    reg [7:0] recv_mem   [0:N*N_BLOCKS-1];
    reg [7:0] exp_mem    [0:K*N_BLOCKS-1];
    reg [7:0] fail_mem   [0:N_BLOCKS-1];
    reg [7:0] errcnt_mem [0:N_BLOCKS-1];
    reg [255:0] f_recv, f_exp, f_fail, f_errcnt;
    integer have_vectors;

    integer blk, i, errors, seed;
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
        seed = 99;
        repeat(3) @(negedge clk); resetn=1;

        for (blk = 0; blk < N_BLOCKS; blk = blk + 1) begin
            recv_base = blk * N;
            exp_base  = blk * K;

            // Drive the 255 input bytes with randomized backpressure on
            // s_valid (occasionally dropping it for a cycle even when the
            // DUT is ready, exactly like every other RTL testbench in this
            // family exercises both directions of backpressure).
            i = 0;
            while (i < N) begin
                @(negedge clk);
                if ({$random(seed)} % 4 == 0) begin
                    s_valid = 0;
                end else begin
                    s_valid = 1;
                    s_data  = recv_mem[recv_base + i];
                end
                @(posedge clk);
                if (s_valid && s_ready) i = i + 1;
            end
            @(negedge clk); s_valid = 0;

            // Drain the 223 output bytes with randomized backpressure on
            // m_ready too.
            i = 0;
            while (i < K) begin
                @(negedge clk);
                m_ready = ({$random(seed)} % 4 != 0);
                @(posedge clk);
                if (m_valid && m_ready) begin
                    if (m_data !== exp_mem[exp_base + i]) begin
                        $display("block %0d byte %0d MISMATCH: got %02h expected %02h",
                                  blk, i, m_data, exp_mem[exp_base + i]);
                        errors = errors + 1;
                    end
                    if (i == K-1) begin
                        if (!m_last) begin
                            $display("block %0d: m_last not set on final byte", blk);
                            errors = errors + 1;
                        end
                        if (fail !== fail_mem[blk][0]) begin
                            $display("block %0d: fail=%b expected %b", blk, fail, fail_mem[blk][0]);
                            errors = errors + 1;
                        end
                        if (error_count !== errcnt_mem[blk][5:0]) begin
                            $display("block %0d: error_count=%0d expected %0d",
                                      blk, error_count, errcnt_mem[blk]);
                            errors = errors + 1;
                        end
                    end
                    i = i + 1;
                end
            end
            @(negedge clk); m_ready = 0;
        end

        if (errors == 0)
            $display("PASS: %0d blocks (clean/corrected/uncorrectable mix), full RS decoder chain", N_BLOCKS);
        else
            $fatal(1, "%0d errors", errors);
        $finish;
    end
    initial begin #20000000; $fatal(1,"timeout"); end
endmodule
