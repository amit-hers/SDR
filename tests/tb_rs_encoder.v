`timescale 1ns/1ps
module tb_rs_encoder;
    localparam integer K = 223, NPAR = 32, N = K + NPAR;
    localparam integer N_BLOCKS = 5;

    reg clk=0; always #5 clk=~clk;
    reg resetn=0;
    reg [7:0] s_data=0; reg s_valid=0;
    wire s_ready;
    wire [7:0] m_data; wire m_valid, m_last;
    reg m_ready=0;

    rs_encoder dut(clk, resetn, s_data, s_valid, s_ready, m_data, m_valid, m_ready, m_last);

    reg [7:0] data_mem [0:N_BLOCKS*K-1];
    reg [7:0] exp_mem  [0:N_BLOCKS*N-1];
    reg [255:0] data_file, exp_file;
    integer have_vectors;

    integer din_idx, dout_idx, byte_in_block, errors;
    integer seed;

    initial begin
        have_vectors = $value$plusargs("data=%s", data_file);
        if (!have_vectors) $fatal(1, "need +data=<file> +expected=<file>");
        have_vectors = $value$plusargs("expected=%s", exp_file);
        if (!have_vectors) $fatal(1, "need +expected=<file>");
        $readmemh(data_file, data_mem);
        $readmemh(exp_file, exp_mem);

        seed = 1;
        errors = 0;
        din_idx = 0; dout_idx = 0; byte_in_block = 0;
        repeat (3) @(negedge clk); resetn = 1;

        // Drive input with randomized stalls (s_valid sometimes low even
        // with data available), independent of the output-side stalling
        // driven by the m_ready process below.
        fork
            begin: feed_proc
                while (din_idx < N_BLOCKS*K) begin
                    @(negedge clk);
                    if ($random(seed) % 4 == 0) begin
                        s_valid <= 0;
                    end else begin
                        s_valid <= 1;
                        s_data  <= data_mem[din_idx];
                    end
                    @(posedge clk);
                    if (s_valid && s_ready) din_idx = din_idx + 1;
                end
                @(negedge clk); s_valid <= 0;
            end
            begin: check_proc
                while (dout_idx < N_BLOCKS*N) begin
                    @(negedge clk);
                    m_ready <= ($random(seed) % 3 != 0);
                    @(posedge clk);
                    if (m_valid && m_ready) begin
                        if (m_data !== exp_mem[dout_idx]) begin
                            $display("MISMATCH byte %0d: got %02h expected %02h",
                                     dout_idx, m_data, exp_mem[dout_idx]);
                            errors = errors + 1;
                        end
                        if (byte_in_block == N-1) begin
                            if (!m_last) begin
                                $display("MISSING m_last at byte %0d", dout_idx);
                                errors = errors + 1;
                            end
                            byte_in_block = 0;
                        end else begin
                            if (m_last) begin
                                $display("UNEXPECTED m_last at byte %0d (block pos %0d)",
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
            $display("PASS: %0d blocks, %0d bytes, randomized input/output stalls, all codewords matched", N_BLOCKS, N_BLOCKS*N);
        else
            $fatal(1, "%0d mismatches", errors);
        $finish;
    end
    initial begin #2000000; $fatal(1, "timeout"); end
endmodule
