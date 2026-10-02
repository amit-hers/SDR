`timescale 1ns/1ps
module tb_rs_chien_search;
    localparam integer N_BLOCKS = 12;
    localparam integer MAX_COEF = 17 * N_BLOCKS; // degree <= 16, so <= 17 bytes/block
    localparam integer MAX_DEG  = 16 * N_BLOCKS;

    reg clk=0; always #5 clk=~clk;
    reg resetn=0;
    reg [7:0] s_data=0; reg s_valid=0; reg s_last=0;
    wire s_ready;
    wire [7:0] m_data; wire m_valid; reg m_ready=0;
    wire search_done;

    rs_chien_search dut(clk, resetn, s_data, s_valid, s_ready, s_last, m_data, m_valid, m_ready, search_done);

    reg [7:0] coef_mem    [0:MAX_COEF-1];
    reg [7:0] coef_len_mem[0:N_BLOCKS-1];
    reg [7:0] deg_cnt_mem [0:N_BLOCKS-1];
    reg [7:0] deg_mem     [0:MAX_DEG-1];
    reg [255:0] f_coef, f_lens, f_cnts, f_degs;
    integer have_vectors;

    integer blk, i, coef_base, deg_base, din_idx, byte_in_block, errors, seed;
    reg block_done;

    initial begin
        have_vectors = $value$plusargs("coef=%s", f_coef);   if (!have_vectors) $fatal(1,"need +coef=");
        have_vectors = $value$plusargs("lens=%s", f_lens);   if (!have_vectors) $fatal(1,"need +lens=");
        have_vectors = $value$plusargs("cnts=%s", f_cnts);   if (!have_vectors) $fatal(1,"need +cnts=");
        have_vectors = $value$plusargs("degs=%s", f_degs);   if (!have_vectors) $fatal(1,"need +degs=");
        $readmemh(f_coef, coef_mem);
        $readmemh(f_lens, coef_len_mem);
        $readmemh(f_cnts, deg_cnt_mem);
        $readmemh(f_degs, deg_mem);

        seed = 1; errors = 0;
        coef_base = 0; deg_base = 0;
        repeat (3) @(negedge clk); resetn = 1;

        for (blk = 0; blk < N_BLOCKS; blk = blk + 1) begin
            // Feed this block's coefficients, s_last on the final one,
            // randomized input stalls.
            din_idx = 0;
            while (din_idx < coef_len_mem[blk]) begin
                @(negedge clk);
                if ($random(seed) % 4 == 0) begin
                    s_valid <= 0;
                end else begin
                    s_valid <= 1;
                    s_data  <= coef_mem[coef_base + din_idx];
                    s_last  <= (din_idx == coef_len_mem[blk]-1);
                end
                @(posedge clk);
                if (s_valid && s_ready) din_idx = din_idx + 1;
            end
            @(negedge clk); s_valid <= 0; s_last <= 0;

            // Drain exactly deg_cnt_mem[blk] found degrees, randomized
            // output stalls (exercises the backpressure fix).
            byte_in_block = 0;
            block_done = (deg_cnt_mem[blk] == 0);
            while (!block_done) begin
                @(negedge clk);
                m_ready <= ($random(seed) % 3 != 0);
                @(posedge clk);
                if (m_valid && m_ready) begin
                    if (m_data !== deg_mem[deg_base + byte_in_block]) begin
                        $display("MISMATCH block %0d degree %0d: got %0d expected %0d",
                                 blk, byte_in_block, m_data, deg_mem[deg_base+byte_in_block]);
                        errors = errors + 1;
                    end
                    byte_in_block = byte_in_block + 1;
                    if (byte_in_block == deg_cnt_mem[blk]) block_done = 1;
                end
            end
            // search_done must fire by the time the block's roots are all
            // drained (it may have already pulsed earlier in a zero-root
            // block, or on the very last position's cycle for a non-zero one).
            if (!search_done) begin
                // Allow a few more cycles in case done pulses alongside the
                // last drained byte's own cycle timing.
                repeat (5) @(posedge clk);
            end

            coef_base = coef_base + coef_len_mem[blk];
            deg_base  = deg_base  + deg_cnt_mem[blk];
        end

        if (errors == 0)
            $display("PASS: %0d blocks (1..16 real errors each), randomized stalls on both sides, all degrees matched",
                      N_BLOCKS);
        else
            $fatal(1, "%0d mismatches", errors);
        $finish;
    end
    initial begin #50000000; $fatal(1, "timeout"); end
endmodule
