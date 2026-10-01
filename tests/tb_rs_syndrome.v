`timescale 1ns/1ps
module tb_rs_syndrome;
    localparam integer N = 255, NSYN = 32;
    localparam integer N_BLOCKS = 8;

    reg clk=0; always #5 clk=~clk;
    reg resetn=0;
    reg [7:0] s_data=0; reg s_valid=0;
    wire s_ready;
    wire [7:0] m_data; wire m_valid, m_last, all_zero;
    reg m_ready=0;

    rs_syndrome dut(clk, resetn, s_data, s_valid, s_ready, m_data, m_valid, m_ready, m_last, all_zero);

    reg [7:0] recv_mem [0:N_BLOCKS*N-1];
    reg [7:0] synd_mem [0:N_BLOCKS*NSYN-1];
    reg [255:0] recv_file, synd_file;
    integer have_vectors;

    integer din_idx, dout_idx, byte_in_block, errors, seed, blk;
    reg block_has_error;

    initial begin
        have_vectors = $value$plusargs("recv=%s", recv_file);
        if (!have_vectors) $fatal(1, "need +recv=<file> +synd=<file>");
        have_vectors = $value$plusargs("synd=%s", synd_file);
        if (!have_vectors) $fatal(1, "need +synd=<file>");
        $readmemh(recv_file, recv_mem);
        $readmemh(synd_file, synd_mem);

        seed = 1;
        errors = 0;
        din_idx = 0; dout_idx = 0; byte_in_block = 0; blk = 0;
        repeat (3) @(negedge clk); resetn = 1;

        fork
            begin: feed_proc
                while (din_idx < N_BLOCKS*N) begin
                    @(negedge clk);
                    if ($random(seed) % 4 == 0) begin
                        s_valid <= 0;
                    end else begin
                        s_valid <= 1;
                        s_data  <= recv_mem[din_idx];
                    end
                    @(posedge clk);
                    if (s_valid && s_ready) din_idx = din_idx + 1;
                end
                @(negedge clk); s_valid <= 0;
            end
            begin: check_proc
                while (dout_idx < N_BLOCKS*NSYN) begin
                    @(negedge clk);
                    m_ready <= ($random(seed) % 3 != 0);
                    @(posedge clk);
                    if (m_valid && m_ready) begin
                        if (m_data !== synd_mem[dout_idx]) begin
                            $display("MISMATCH syndrome %0d (block %0d, S_%0d): got %02h expected %02h",
                                     dout_idx, blk, byte_in_block, m_data, synd_mem[dout_idx]);
                            errors = errors + 1;
                        end
                        if (byte_in_block == NSYN-1) begin
                            if (!m_last) begin
                                $display("MISSING m_last at syndrome %0d", dout_idx);
                                errors = errors + 1;
                            end
                            // Cross-check all_zero against the vectors themselves:
                            // recompute whether this block's expected syndromes are
                            // all zero and compare to what the DUT asserted.
                            block_has_error = 0;
                            for (integer k = 0; k < NSYN; k = k + 1)
                                if (synd_mem[blk*NSYN + k] != 8'h00) block_has_error = 1;
                            if (all_zero == block_has_error) begin
                                $display("all_zero WRONG for block %0d: dut=%b expected_no_error=%b",
                                         blk, all_zero, !block_has_error);
                                errors = errors + 1;
                            end
                            blk = blk + 1;
                            byte_in_block = 0;
                        end else begin
                            if (m_last) begin
                                $display("UNEXPECTED m_last at syndrome %0d (block pos %0d)",
                                         dout_idx, byte_in_block);
                                errors = errors + 1;
                            end
                            byte_in_block = byte_in_block + 1;
                        end
                        dout_idx = dout_idx + 1;
                    end
                end
            end
        join

        if (errors == 0)
            $display("PASS: %0d blocks, %0d syndrome bytes, randomized stalls, all matched (incl. all_zero flag)",
                      N_BLOCKS, N_BLOCKS*NSYN);
        else
            $fatal(1, "%0d mismatches", errors);
        $finish;
    end
    initial begin #3000000; $fatal(1, "timeout"); end
endmodule
