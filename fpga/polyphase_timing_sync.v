// polyphase_timing_sync
//
// Streaming matched filter + timing recovery + decimation to 1 sample/symbol.
// Bit-exact target: sdr::FixedTimingSync with interp=POLYPHASE, n_phases=32,
// fixed_loop=true, alpha_sh=12, beta_sh=22, phase_offset=0. That model was
// validated at 94.2% CRC on recorded RF (symsync_crcf: 96.2%).
//
//   * 32-phase polyphase RRC bank, 32 taps/phase, Q1.15. The bank IS the
//     matched filter -- no separate FIR stage.
//   * Gardner TED: no symbol decisions, so this sits ahead of carrier
//     recovery. Error power-normalised by a leading-one barrel shift.
//   * Q16.16 NCO; the loop integrator is Q16.32. Carrying the integrator at
//     the NCO's precision truncates beta*err to zero for realistic errors and
//     silently reduces the loop to proportional-only (measured: 0% CRC).
//
// Zynq-7020: 1 BRAM18 for the bank, 32 DSP48 for the MAC (or 8 at 4x
// time-multiplexing, since the symbol rate is 1/4 the sample rate).

`timescale 1ns / 1ps

module polyphase_timing_sync #(
    parameter integer DW       = 16,
    parameter integer NPHASE   = 32,
    parameter integer NTAP     = 32,
    parameter integer LQ       = 16,
    parameter integer EQ       = 24,
    parameter integer FQ       = 32,
    parameter integer SPS      = 4,
    parameter integer ALPHA_SH = 12,
    parameter integer BETA_SH  = 22,
    parameter         COEFF_HEX = "rrc_bank_32x32.hex"
)(
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 s_valid,
    input  wire signed [DW-1:0] s_i,
    input  wire signed [DW-1:0] s_q,
    output reg                  m_valid,
    output reg  signed [DW-1:0] m_i,
    output reg  signed [DW-1:0] m_q,
    // Verification taps: the fields the C++ model also records, so the two
    // can be diffed field-by-field instead of inferring the mapping from
    // output values. Synthesis prunes these when unconnected.
    output reg  signed [63:0]   t_base,
    output reg         [31:0]   t_mu,
    output reg         [PHW-1:0] t_phase,
    output reg  signed [63:0]   t_hbase,
    output reg         [PHW-1:0] t_hphase,
    output reg  signed [63:0]   t_raw_e,
    output reg  signed [63:0]   t_pwr,
    output reg  signed [63:0]   t_e_q
);
    localparam integer PHW = 5;   // log2(NPHASE)
    reg [PHW-1:0] mid_phase_r;
    reg signed [63:0] mid_base_r;

    reg signed [DW-1:0] coeff [0:NPHASE*NTAP-1];
    initial $readmemh(COEFF_HEX, coeff);

    reg signed [DW-1:0] hist_i [0:NTAP-1];
    reg signed [DW-1:0] hist_q [0:NTAP-1];

    reg signed [63:0] sidx;    // absolute index of the sample at hist[0]
    reg signed [63:0] pos_q;   // Q16.16 absolute index of next symbol
    reg signed [63:0] freq_q;  // Q16.32 loop integrator

    reg signed [DW-1:0] prev_i, prev_q, mid_i, mid_q;
    reg have_prev, have_mid;

    // The history must be primed before any output is meaningful. In C++ the
    // vectors were zero-initialised and the model simply started past the
    // filter transient; in fabric every register powers up undefined, so the
    // MAC would emit x until the shift register happened to fill -- and worse,
    // an x in prev/mid propagates into the NCO through the TED and destroys
    // the timing loop permanently.
    wire hist_primed = (sidx >= NTAP);

    wire signed [63:0] hp_q  = pos_q - (SPS <<< (LQ-1));
    wire signed [63:0] base  = pos_q >>> LQ;
    wire signed [63:0] hbase = hp_q  >>> LQ;
    // Truncating shift. Rounding was tried (adding half an LSB before the
    // shift, to mirror the model's llround) and measured WORSE: bit-exact
    // agreement fell 60.6% -> 22.9% and rms error rose 437 -> 1045. So the
    // model's effective phase is not simply llround(mu*NPHASE); the mapping
    // is being determined by direct trace comparison rather than inferred.
    wire [PHW-1:0] ph_cur = (pos_q >>> (LQ-PHW)) & (NPHASE-1);
    wire [PHW-1:0] ph_mid = (hp_q  >>> (LQ-PHW)) & (NPHASE-1);

    function signed [DW-1:0] mac;
        input [PHW-1:0] ph;
        input integer   sel;
        integer t;
        reg signed [63:0] a;
        begin
            a = 0;
            for (t = 0; t < NTAP; t = t + 1)
                a = a + $signed(coeff[ph*NTAP + t]) *
                        (sel ? $signed(hist_q[t]) : $signed(hist_i[t]));
            a = a >>> 15;
            if (a >  32767) a =  32767;
            if (a < -32768) a = -32768;
            mac = a[DW-1:0];
        end
    endfunction

    // The MAC is evaluated procedurally, not through a continuous assign.
    // A function used in `assign` only re-evaluates when its explicit
    // arguments change -- it is not sensitive to the hist arrays it reads --
    // so the filter output would freeze at whatever it held when the phase
    // index last moved. Symptom: every emitted symbol reads 0.
    reg signed [DW-1:0] cur_i, cur_q;
    reg signed [63:0] di, dq, raw_e, pwr;

    function integer msb_pos;
        input signed [63:0] v;
        integer i;
        begin
            msb_pos = -1;
            for (i = 0; i < 48; i = i + 1) if (v[i]) msb_pos = i;
        end
    endfunction

    integer pm, k;
    reg signed [63:0] e_q, lim, fnext;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sidx      <= 0;
            pos_q     <= (NTAP <<< LQ) + (SPS <<< LQ);
            freq_q    <= 0;
            have_prev <= 1'b0;
            have_mid  <= 1'b0;
            m_valid   <= 1'b0;
            m_i       <= 0;
            m_q       <= 0;
            prev_i    <= 0;
            prev_q    <= 0;
            mid_i     <= 0;
            mid_q     <= 0;
            t_base    <= 0;  t_mu     <= 0;  t_phase  <= 0;
            t_hbase   <= 0;  t_hphase <= 0;
            t_raw_e   <= 0;  t_pwr    <= 0;  t_e_q    <= 0;
            for (k = 0; k < NTAP; k = k + 1) begin
                hist_i[k] <= 0;
                hist_q[k] <= 0;
            end
        end else begin
            m_valid <= 1'b0;
            if (s_valid) begin
                if (sidx == hbase && hist_primed) begin
                    mid_i       <= mac(ph_mid, 0);
                    mid_q       <= mac(ph_mid, 1);
                    have_mid    <= 1'b1;
                    mid_phase_r <= ph_mid;
                    mid_base_r  <= hbase;
                end

                if (sidx == base && hist_primed) begin
                    cur_i = mac(ph_cur, 0);
                    cur_q = mac(ph_cur, 1);
                    di    = cur_i - prev_i;
                    dq    = cur_q - prev_q;
                    raw_e = $signed(mid_i)*di + $signed(mid_q)*dq;
                    pwr   = ($signed(cur_i)*$signed(cur_i) + $signed(cur_q)*$signed(cur_q)
                           + $signed(prev_i)*$signed(prev_i) + $signed(prev_q)*$signed(prev_q)) >>> 1;

                    m_i     <= cur_i;
                    m_q     <= cur_q;
                    m_valid <= 1'b1;
                    t_base   <= base;
                    t_mu     <= pos_q[LQ-1:0];
                    t_phase  <= ph_cur;
                    t_hbase  <= mid_base_r;
                    t_hphase <= mid_phase_r;

                    // The loop only runs once prev and mid are real samples.
                    // Updating it earlier feeds the NCO an error derived from
                    // reset values, which biases the very first correction and
                    // takes many symbols to wash out.
                    if (have_prev && have_mid && pwr != 0) begin
                        pm  = msb_pos(pwr);
                        e_q = (pm > EQ) ? (raw_e >>> (pm - EQ)) : (raw_e <<< (EQ - pm));
                        lim = (64'sd1 <<< EQ);
                        if (e_q >  lim) e_q =  lim;
                        if (e_q < -lim) e_q = -lim;
                        fnext  = freq_q - (e_q >>> BETA_SH);
                        t_raw_e <= raw_e;
                        t_pwr   <= pwr;
                        t_e_q   <= e_q;
                        freq_q <= fnext;
                        pos_q  <= pos_q + (SPS <<< LQ) + (fnext >>> (FQ-LQ))
                                        - (e_q >>> ALPHA_SH);
                    end else begin
                        pos_q <= pos_q + (SPS <<< LQ) + (freq_q >>> (FQ-LQ));
                    end

                    prev_i    <= cur_i;
                    prev_q    <= cur_q;
                    have_prev <= 1'b1;
                end

                for (k = NTAP-1; k > 0; k = k - 1) begin
                    hist_i[k] <= hist_i[k-1];
                    hist_q[k] <= hist_q[k-1];
                end
                hist_i[0] <= s_i;
                hist_q[0] <= s_q;
                sidx <= sidx + 1;
            end
        end
    end
endmodule
