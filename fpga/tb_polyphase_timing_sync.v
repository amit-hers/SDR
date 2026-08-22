`timescale 1ns / 1ps
module tb;
    localparam integer NIN = 40000;
    reg clk = 0, rst_n = 0, s_valid = 0;
    reg signed [15:0] s_i, s_q;
    wire m_valid;
    wire signed [15:0] m_i, m_q;
    wire signed [63:0] t_base, t_hbase;
    wire [31:0] t_mu;
    wire [4:0]  t_phase, t_hphase;
    wire signed [63:0] t_raw_e, t_pwr, t_e_q, t_ci, t_cq, t_pi, t_pq, t_mi, t_mq;
    integer ftr, sym;

    reg [15:0] in_i [0:NIN-1];
    reg [15:0] in_q [0:NIN-1];
    integer n, fout;

    polyphase_timing_sync #(.COEFF_HEX("rrc_bank_32x32.hex")) dut (
        .clk(clk), .rst_n(rst_n), .s_valid(s_valid),
        .s_i(s_i), .s_q(s_q), .m_valid(m_valid), .m_i(m_i), .m_q(m_q),
        .t_base(t_base), .t_mu(t_mu), .t_phase(t_phase),
        .t_hbase(t_hbase), .t_hphase(t_hphase),
        .t_raw_e(t_raw_e), .t_pwr(t_pwr), .t_e_q(t_e_q),
        .t_ci(t_ci), .t_cq(t_cq), .t_pi(t_pi), .t_pq(t_pq), .t_mi(t_mi), .t_mq(t_mq));

    always #5 clk = ~clk;

    initial begin
        $readmemh("golden_in_i.hex", in_i);
        $readmemh("golden_in_q.hex", in_q);
        fout = $fopen("hdl_out.hex", "w");
        ftr  = $fopen("hdl_trace.txt", "w");
        $fwrite(ftr, "# n base mu_q16 phase hbase hphase raw_e pwr e_q ci cq pi pq mi mq\n");
        sym = 0;
        @(posedge clk); rst_n = 1;
        for (n = 0; n < NIN; n = n + 1) begin
            @(negedge clk);
            s_valid = 1; s_i = in_i[n]; s_q = in_q[n];
            @(posedge clk);
            #1 if (m_valid) begin
                $fwrite(fout, "%h %h\n", m_i, m_q);
                $fwrite(ftr, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n",
                        sym, t_base, t_mu, t_phase, t_hbase, t_hphase,
                        t_raw_e, t_pwr, t_e_q, t_ci, t_cq, t_pi, t_pq, t_mi, t_mq);
                sym = sym + 1;
            end
        end
        @(negedge clk); s_valid = 0;
        repeat (10) @(posedge clk);
        $fclose(fout); $fclose(ftr);
        $display("tb: done, %0d input samples", NIN);
        $finish;
    end
endmodule
