`timescale 1ns/1ps
module tb_rs_berlekamp_massey;
    localparam integer NPAR = 32;
    localparam integer N_BLOCKS = 10;

    reg clk=0; always #5 clk=~clk;
    reg resetn=0;
    reg [7:0] s_data=0; reg s_valid=0;
    wire s_ready;
    wire [7:0] m_data; wire m_valid, m_last;
    wire [5:0] degree; wire done;
    reg m_ready=0;

    rs_berlekamp_massey dut(clk, resetn, s_data, s_valid, s_ready, m_data, m_valid, m_ready, m_last, degree, done);

    reg [7:0] synd_mem [0:N_BLOCKS*NPAR-1];
    reg [7:0] degree_mem [0:N_BLOCKS-1];
    reg [7:0] coef_mem [0:N_BLOCKS*(NPAR+1)-1];
    reg [255:0] synd_file, degree_file, coef_file;
    integer have_vectors;

    integer din_idx, blk, byte_in_block, errors, seed;
    reg block_done;

    initial begin
        have_vectors = $value$plusargs("synd=%s", synd_file);
        if (!have_vectors) $fatal(1, "need +synd=<file> +degree=<file> +coef=<file>");
        have_vectors = $value$plusargs("degree=%s", degree_file);
        if (!have_vectors) $fatal(1, "need +degree=<file>");
        have_vectors = $value$plusargs("coef=%s", coef_file);
        if (!have_vectors) $fatal(1, "need +coef=<file>");
        $readmemh(synd_file, synd_mem);
        $readmemh(degree_file, degree_mem);
        $readmemh(coef_file, coef_mem);

        seed = 1;
        errors = 0;
        din_idx = 0; blk = 0;
        repeat (3) @(negedge clk); resetn = 1;

        for (blk = 0; blk < N_BLOCKS; blk = blk + 1) begin
            // Feed this block's 32 syndrome bytes, randomized input stalls.
            din_idx = 0;
            while (din_idx < NPAR) begin
                @(negedge clk);
                if ($random(seed) % 4 == 0) begin
                    s_valid <= 0;
                end else begin
                    s_valid <= 1;
                    s_data  <= synd_mem[blk*NPAR + din_idx];
                end
                @(posedge clk);
                if (s_valid && s_ready) din_idx = din_idx + 1;
            end
            @(negedge clk); s_valid <= 0;

            // Drain this block's output: degree + coefficients up to m_last.
            byte_in_block = 0;
            block_done = 0;
            while (!block_done) begin
                @(negedge clk);
                m_ready <= ($random(seed) % 3 != 0);
                @(posedge clk);
                if (m_valid && m_ready) begin
                    if (byte_in_block == 0) begin
                        if (degree !== degree_mem[blk][5:0]) begin
                            $display("DEGREE MISMATCH block %0d: got %0d expected %0d",
                                     blk, degree, degree_mem[blk]);
                            errors = errors + 1;
                        end
                    end
                    if (m_data !== coef_mem[blk*(NPAR+1) + byte_in_block]) begin
                        $display("COEF MISMATCH block %0d byte %0d: got %02h expected %02h",
                                 blk, byte_in_block, m_data, coef_mem[blk*(NPAR+1)+byte_in_block]);
                        errors = errors + 1;
                    end
                    if (m_last) begin
                        if (byte_in_block !== degree_mem[blk]) begin
                            $display("m_last at wrong byte, block %0d: byte %0d expected degree %0d",
                                     blk, byte_in_block, degree_mem[blk]);
                            errors = errors + 1;
                        end
                        block_done = 1;
                    end else begin
                        byte_in_block = byte_in_block + 1;
                    end
                end
            end
        end

        if (errors == 0)
            $display("PASS: %0d blocks (incl. an all-zero/no-error block), randomized stalls, degree+locator all matched",
                      N_BLOCKS);
        else
            $fatal(1, "%0d mismatches", errors);
        $finish;
    end
    initial begin #5000000; $fatal(1, "timeout"); end
endmodule
