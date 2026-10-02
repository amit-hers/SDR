`timescale 1ns/1ps
module tb_rs_forney;
    localparam integer N_BLOCKS = 12;
    localparam integer MAX_COEF = 17 * N_BLOCKS;
    localparam integer MAX_SYND = 32 * N_BLOCKS;
    localparam integer MAX_DEG  = 16 * N_BLOCKS;

    reg clk=0; always #5 clk=~clk;
    reg resetn=0;
    reg [7:0] cs_data=0; reg cs_valid=0; reg c_last=0; reg cs_last=0;
    wire cs_ready;
    reg [7:0] d_data=0; reg d_valid=0;
    wire d_ready;
    wire [7:0] pos_data, mag_data; wire m_valid; reg m_ready=0;
    wire forney_fail;

    rs_forney dut(clk, resetn, cs_data, cs_valid, cs_ready, c_last, cs_last,
                  d_data, d_valid, d_ready, pos_data, mag_data, m_valid, m_ready, forney_fail);

    reg [7:0] coef_mem    [0:MAX_COEF-1];
    reg [7:0] coef_len_mem[0:N_BLOCKS-1];
    reg [7:0] synd_mem    [0:MAX_SYND-1];
    reg [7:0] deg_cnt_mem [0:N_BLOCKS-1];
    reg [7:0] deg_mem     [0:MAX_DEG-1];
    reg [7:0] mag_mem     [0:MAX_DEG-1];
    reg [255:0] f_coef, f_lens, f_synd, f_cnts, f_degs, f_mags;
    integer have_vectors;

    integer blk, i, coef_base, synd_base, deg_base, din_idx, errors, seed;
    integer got_count;
    reg block_done;

    initial begin
        have_vectors=$value$plusargs("coef=%s",f_coef); if(!have_vectors) $fatal(1,"need +coef=");
        have_vectors=$value$plusargs("lens=%s",f_lens); if(!have_vectors) $fatal(1,"need +lens=");
        have_vectors=$value$plusargs("synd=%s",f_synd); if(!have_vectors) $fatal(1,"need +synd=");
        have_vectors=$value$plusargs("cnts=%s",f_cnts); if(!have_vectors) $fatal(1,"need +cnts=");
        have_vectors=$value$plusargs("degs=%s",f_degs); if(!have_vectors) $fatal(1,"need +degs=");
        have_vectors=$value$plusargs("mags=%s",f_mags); if(!have_vectors) $fatal(1,"need +mags=");
        $readmemh(f_coef, coef_mem);
        $readmemh(f_lens, coef_len_mem);
        $readmemh(f_synd, synd_mem);
        $readmemh(f_cnts, deg_cnt_mem);
        $readmemh(f_degs, deg_mem);
        $readmemh(f_mags, mag_mem);

        seed=1; errors=0;
        coef_base=0; synd_base=0; deg_base=0;
        repeat(3) @(negedge clk); resetn=1;

        for (blk=0; blk<N_BLOCKS; blk=blk+1) begin
            // Feed C[0..degree], c_last on the final byte.
            din_idx=0;
            while (din_idx < coef_len_mem[blk]) begin
                @(negedge clk);
                if ($random(seed)%4==0) begin
                    cs_valid<=0;
                end else begin
                    cs_valid<=1; cs_data<=coef_mem[coef_base+din_idx];
                    c_last<=(din_idx==coef_len_mem[blk]-1); cs_last<=0;
                end
                @(posedge clk);
                if (cs_valid && cs_ready) din_idx=din_idx+1;
            end
            @(negedge clk); cs_valid<=0; c_last<=0;

            // Feed exactly 32 syndromes, cs_last on the final one.
            din_idx=0;
            while (din_idx < 32) begin
                @(negedge clk);
                if ($random(seed)%4==0) begin
                    cs_valid<=0;
                end else begin
                    cs_valid<=1; cs_data<=synd_mem[synd_base+din_idx];
                    cs_last<=(din_idx==31);
                end
                @(posedge clk);
                if (cs_valid && cs_ready) din_idx=din_idx+1;
            end
            @(negedge clk); cs_valid<=0; cs_last<=0;

            // Feed each found degree, one at a time, collecting (pos, mag)
            // as they come back -- interleaved, since the DUT only accepts
            // the next request after finishing (and draining) the current one.
            got_count=0;
            while (got_count < deg_cnt_mem[blk]) begin
                // Push one degree request (with its own small stall chance).
                block_done=0;
                while (!block_done) begin
                    @(negedge clk);
                    if ($random(seed)%4==0) d_valid<=0;
                    else begin d_valid<=1; d_data<=deg_mem[deg_base+got_count]; end
                    @(posedge clk);
                    if (d_valid && d_ready) block_done=1;
                end
                @(negedge clk); d_valid<=0;
                // Drain the resulting (pos, mag) pair, randomized stalls.
                block_done=0;
                while (!block_done) begin
                    @(negedge clk);
                    m_ready <= ($random(seed)%3!=0);
                    @(posedge clk);
                    if (m_valid && m_ready) begin
                        if (pos_data !== deg_mem[deg_base+got_count]) begin
                            $display("POS MISMATCH block %0d req %0d: got %0d expected %0d",
                                     blk, got_count, pos_data, deg_mem[deg_base+got_count]);
                            errors=errors+1;
                        end
                        if (forney_fail) begin
                            $display("UNEXPECTED forney_fail block %0d req %0d", blk, got_count);
                            errors=errors+1;
                        end else if (mag_data !== mag_mem[deg_base+got_count]) begin
                            $display("MAG MISMATCH block %0d req %0d (pos %0d): got %02h expected %02h",
                                     blk, got_count, pos_data, mag_data, mag_mem[deg_base+got_count]);
                            errors=errors+1;
                        end
                        block_done=1;
                    end
                end
                got_count=got_count+1;
            end

            coef_base=coef_base+coef_len_mem[blk];
            synd_base=synd_base+32;
            deg_base=deg_base+deg_cnt_mem[blk];
        end

        if (errors==0)
            $display("PASS: %0d blocks, full real pipeline (encode+corrupt+syndrome+BM+Chien+Forney), all magnitudes matched",
                      N_BLOCKS);
        else
            $fatal(1, "%0d mismatches", errors);
        $finish;
    end
    initial begin #100000000; $fatal(1,"timeout"); end
endmodule
