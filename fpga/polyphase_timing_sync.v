// polyphase_timing_sync
//
// Streaming matched filter + timing recovery + decimation to 1 sample/symbol.
// Bit-exact target: sdr::FixedTimingSync with interp=POLYPHASE, n_phases=32,
// fixed_loop=true, alpha_sh=12, beta_sh=22, phase_offset=0. That model was
// validated at 94.2% CRC on recorded RF (symsync_crcf: 96.2%).
//
//   * 32-phase polyphase RRC bank, 32 taps/phase, Q1.15 coefficients over
//     Q1.15 input. The bank IS the matched filter -- no separate FIR stage.
//     Its OUTPUT is 20-bit, not Q1.15: the filter has processing gain.
//   * Gardner TED: no symbol decisions, so this sits ahead of carrier
//     recovery. Error normalised by an exact divide, not a barrel shift --
//     the shift approximation was off by up to 2x and pinned the error at
//     its clamp on most symbols.
//   * Q16.16 NCO; the loop integrator is Q16.32. Carrying the integrator at
//     the NCO's precision truncates beta*err to zero for realistic errors and
//     silently reduces the loop to proportional-only (measured: 0% CRC).
//
// Verified bit-exact against the golden model on 9990 symbols of recorded RF:
// every datapath field (base, mu, phase, cur/prev/mid, raw_e, e_q, and all
// eight NCO intermediates) matches on every symbol. Reproduce with
//   iverilog -g2005 -o tb.vvp tb_polyphase_timing_sync.v polyphase_timing_sync.v
//   vvp tb.vvp && python3 tdiff.py golden_trace.txt hdl_trace.txt
//
// Zynq-7020: 1 BRAM18 for the bank, 32 DSP48 for the MAC (or 8 at 4x
// time-multiplexing, since the symbol rate is 1/4 the sample rate). The
// exact divider is the one piece with no obvious cheap mapping; four sample
// clocks per symbol leaves room for a pipelined divide or reciprocal LUT.

`timescale 1ns / 1ps

module polyphase_timing_sync #(
    parameter integer DW       = 16,
    // Internal matched-filter output width. The bank has processing gain, so
    // its output does NOT fit in Q1.15 even though its input does: on the
    // validation capture 80% of the filtered I samples fall outside int16
    // (peak |value| 86718, an 18-bit signed quantity).
    //
    // The C++ model never saturates here -- satQ15 is applied only in the
    // FARROW path's separate FIR stage, which polyphase mode bypasses. Making
    // mac() clamp to Q1.15 therefore fed the Gardner TED clipped samples and
    // corrupted the loop; it first bit at symbol 554 (mid_q -33011 -> -32768)
    // and diverged permanently from there.
    //
    // 20 bits gives ~7.5x headroom over the observed peak. The output PORT is
    // still Q1.15 and saturates, which matches the reference vectors -- those
    // are written through a clamping q15() -- but the clamp must not be in
    // the feedback path.
    parameter integer MW       = 20,
    parameter integer NPHASE   = 32,
    parameter integer NTAP     = 32,
    // Start position, in samples, of the first symbol instant. The C++ model
    // derives this from the RRC *prototype* length, which is sps*span+1 = 33,
    // while sizing its polyphase bank from RRC_TAPS = 32. Mirroring only the
    // bank size here started the NCO one sample early and biased `base` by +1
    // on 93% of symbols -- the first and largest divergence in the traces.
    parameter integer START_OFF = 33,
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
    // Full-precision symbol output, NOT Q1.15. The matched filter has
    // processing gain and 88% of symbols on the validation capture exceed
    // int16; narrowing this port to DW would clamp nearly every symbol and
    // destroy the constellation downstream. The reference vector file is
    // Q1.15 only because its exporter clamps on write, so the testbench
    // applies that same clamp when comparing -- the datapath must not.
    output reg  signed [MW-1:0] m_i,
    output reg  signed [MW-1:0] m_q,
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
    output reg  signed [63:0]   t_e_q,
    output reg  signed [63:0]   t_ci, t_cq, t_pi, t_pq, t_mi, t_mq,
    output reg  signed [63:0]   t_posb, t_spsq, t_freqb, t_ebeta,
                                t_freqa, t_freqsh, t_ealpha, t_posa
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

    reg signed [MW-1:0] prev_i, prev_q, mid_i, mid_q;
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
    // The model picks the phase NEAREST mu, and lets it wrap without carrying
    // into base:
    //     p = llround(mu * NPHASE) % NPHASE;
    // so mu = 0.9888 gives llround(31.64) = 32 -> phase 0, while base stays
    // put. Truncating instead picked phase 31 there and sampled the wrong
    // sub-filter on ~6% of symbols.
    //
    // The rounding must be applied to the FRACTIONAL field on its own. An
    // earlier attempt added the half-LSB to the whole of pos_q before the
    // split, which let the carry reach base as well -- that advances the
    // sample index by one on top of the phase wrap, and measured far worse
    // than plain truncation (60.6% -> 22.9%). Masking the sum to PHW bits
    // both wraps the phase and discards the carry, which is exactly the
    // model's `% NPHASE` with base untouched.
    localparam integer PH_HALF = 1 << (LQ-PHW-1);
    wire [LQ-1:0] frac_cur = pos_q[LQ-1:0];
    wire [LQ-1:0] frac_mid = hp_q [LQ-1:0];
    wire [PHW-1:0] ph_cur = ((frac_cur + PH_HALF) >> (LQ-PHW)) & (NPHASE-1);
    wire [PHW-1:0] ph_mid = ((frac_mid + PH_HALF) >> (LQ-PHW)) & (NPHASE-1);

    // Tap 0 is the sample arriving THIS cycle, not hist[0].
    //
    // hist is written with a non-blocking assignment at the end of the same
    // clocked block that evaluates this MAC, so hist[0] still holds the
    // PREVIOUS sample when the MAC runs. Reading taps straight out of hist
    // therefore convolves a window one sample behind the model's
    // wi[base-k] -- which at the first symbol meant MACing over reset zeros
    // and emitting cur=(0,0) where the model produced (-411,615). Every
    // later divergence (prev, mid, raw_e, and finally the NCO at symbol 2)
    // followed from that single value.
    function signed [MW-1:0] mac;
        input [PHW-1:0] ph;
        input integer   sel;
        integer t;
        reg signed [63:0] a;
        reg signed [DW-1:0] x;
        begin
            a = 0;
            for (t = 0; t < NTAP; t = t + 1) begin
                x = (t == 0) ? (sel ? s_q : s_i)
                             : (sel ? hist_q[t-1] : hist_i[t-1]);
                a = a + $signed(coeff[ph*NTAP + t]) * $signed(x);
            end
            a = a >>> 15;
            // Saturate at the internal width, not at Q1.15. This bound is
            // never reached on the validation capture; it exists so the
            // fabric behaviour is defined on abnormally hot input rather
            // than wrapping sign.
            if (a >  ((64'sd1 <<< (MW-1)) - 1)) a =  (64'sd1 <<< (MW-1)) - 1;
            if (a < -(64'sd1 <<< (MW-1)))       a = -(64'sd1 <<< (MW-1));
            mac = a[MW-1:0];
        end
    endfunction

    // Q1.15 output port: the reference vectors are written through a clamping
    // q15(), so the port clamps identically.
    function signed [DW-1:0] sat16;
        input signed [MW-1:0] v;
        begin
            if (v >  32767) sat16 =  16'sd32767;
            else if (v < -32768) sat16 = -16'sd32768;
            else sat16 = v[DW-1:0];
        end
    endfunction

    // The MAC is evaluated procedurally, not through a continuous assign.
    // A function used in `assign` only re-evaluates when its explicit
    // arguments change -- it is not sensitive to the hist arrays it reads --
    // so the filter output would freeze at whatever it held when the phase
    // index last moved. Symptom: every emitted symbol reads 0.
    reg signed [MW-1:0] cur_i, cur_q;
    reg signed [63:0] di, dq, raw_e, pwr, psum;

    function integer msb_pos;
        input signed [63:0] v;
        integer i;
        begin
            msb_pos = -1;
            for (i = 0; i < 48; i = i + 1) if (v[i]) msb_pos = i;
        end
    endfunction

    integer pm, k;
    reg signed [63:0] e_q, lim, fnext, ebeta, fsh, ealpha, pnext;

    // Held in a 64-bit signed localparam rather than written inline as
    // (SPS <<< LQ). Verilog makes a whole expression unsigned if any operand
    // is, and an unsigned SPS_Q would turn the pos_q update -- which must
    // track negative corrections -- into modular arithmetic on a 64-bit
    // magnitude. The C++ model does this in int64_t throughout.
    localparam signed [63:0] SPS_Q = SPS * (1 << LQ);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sidx      <= 0;
            pos_q     <= (START_OFF <<< LQ) + (SPS <<< LQ);
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
            t_ci <= 0; t_cq <= 0; t_pi <= 0; t_pq <= 0; t_mi <= 0; t_mq <= 0;
            t_posb <= 0; t_spsq <= 0; t_freqb <= 0; t_ebeta <= 0;
            t_freqa <= 0; t_freqsh <= 0; t_ealpha <= 0; t_posa <= 0;
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
                    psum  = $signed(cur_i)*$signed(cur_i) + $signed(cur_q)*$signed(cur_q)
                          + $signed(prev_i)*$signed(prev_i) + $signed(prev_q)*$signed(prev_q);
                    pwr   = psum >>> 1;

                    m_i     <= cur_i;
                    m_q     <= cur_q;
                    m_valid <= 1'b1;
                    t_base   <= base;
                    t_mu     <= pos_q[LQ-1:0];
                    t_phase  <= ph_cur;
                    t_hbase  <= mid_base_r;
                    t_hphase <= mid_phase_r;
                    // Driven on every emitted symbol, not just the ones where
                    // the loop runs. Gating these behind have_prev made symbol
                    // 0 report its reset value (0,0) against the model's
                    // (-411,615) and looked for a while like a MAC fault.
                    t_ci <= cur_i;  t_cq <= cur_q;
                    t_pi <= prev_i; t_pq <= prev_q;
                    t_mi <= mid_i;  t_mq <= mid_q;

                    // The loop only runs once prev and mid are real samples.
                    // Updating it earlier feeds the NCO an error derived from
                    // reset values, which biases the very first correction and
                    // takes many symbols to wash out.
                    if (have_prev && have_mid && psum > 2) begin
                        // Exact division, not a power-of-two barrel shift.
                        // The shift approximated raw_e/pwr by raw_e >> msb(pwr),
                        // which is off by up to 2x per symbol and pinned e_q at
                        // the +-1.0 clamp on most symbols (measured ratio to the
                        // reference: median 1.29, range -25.9 .. +18.7).
                        //
                        // psum is 2*pwr, so dividing by it with one extra bit of
                        // shift avoids the >>1 truncation the model does not do
                        // (its power is a double, 0.5*sum).
                        //
                        // Four sample-clocks per symbol leaves room for a
                        // pipelined divider or a reciprocal-LUT multiply.
                        e_q = (raw_e <<< (EQ+1)) / psum;
                        lim = (64'sd1 <<< EQ);
                        if (e_q >  lim) e_q =  lim;
                        if (e_q < -lim) e_q = -lim;
                        // Same statement order as the C++ model: beta is
                        // applied to freq FIRST, and the freq that feeds the
                        // position update is the POST-beta value. Shifting the
                        // pre-beta freq instead leaves the integrator one
                        // symbol stale and shows up as a slow mu drift.
                        ebeta  = e_q >>> BETA_SH;
                        fnext  = freq_q - ebeta;
                        fsh    = fnext >>> (FQ-LQ);
                        ealpha = e_q >>> ALPHA_SH;
                        pnext  = pos_q + SPS_Q + fsh - ealpha;

                        t_raw_e <= raw_e;
                        t_pwr   <= pwr;
                        t_e_q   <= e_q;
                        t_posb  <= pos_q;   t_spsq   <= SPS_Q;
                        t_freqb <= freq_q;  t_ebeta  <= ebeta;
                        t_freqa <= fnext;   t_freqsh <= fsh;
                        t_ealpha<= ealpha;  t_posa   <= pnext;

                        freq_q <= fnext;
                        pos_q  <= pnext;
                    end else begin
                        // The model has no such branch -- it runs the update
                        // unconditionally with e_q = 0, which reduces to this.
                        // Taps are still driven so the trace lines up symbol
                        // for symbol with the model's, which records these
                        // fields on every symbol.
                        fsh   = freq_q >>> (FQ-LQ);
                        pnext = pos_q + SPS_Q + fsh;
                        t_posb  <= pos_q;   t_spsq   <= SPS_Q;
                        t_freqb <= freq_q;  t_ebeta  <= 64'sd0;
                        t_freqa <= freq_q;  t_freqsh <= fsh;
                        t_ealpha<= 64'sd0;  t_posa   <= pnext;
                        pos_q <= pnext;
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
