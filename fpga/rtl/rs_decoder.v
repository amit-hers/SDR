// RS(255,223) FULL DECODER: chains rs_syndrome -> rs_berlekamp_massey ->
// rs_chien_search -> rs_forney automatically in hardware, replacing the
// manual CPU-driven sequencing axi_rs_test_harness_hw.v does one AXI-Lite
// register poke at a time. Takes one 255-byte received codeword in, emits
// 223 corrected (or passthrough, or best-effort uncorrected-on-failure)
// bytes out, plus a `fail` flag and an `error_count` telemetry counter.
//
// Confirmed before writing this that none of the four sub-blocks need an
// explicit reset pulse between codewords -- each one auto-rearms its own
// input-collection phase once its output stream fully drains (rs_syndrome
// and rs_berlekamp_massey: `in_data<=1` when the last output byte is
// accepted; rs_chien_search: same, when the 255-position search ends;
// rs_forney: ST_READY accepts a fresh cs_valid to restart ST_COEF). So this
// module instantiates all four ONCE with one shared `resetn`, and simply
// sequences through states -- no per-codeword sub-block reset needed.
//
// Buffer reuse is the one genuinely new risk here (every sub-block's own
// algorithm is already proven correct): syndromes are needed TWICE (once to
// feed Berlekamp-Massey, once to feed Forney's second input phase), and the
// error-locator coefficients are needed TWICE too (once for Chien search,
// once for Forney's first input phase) -- each sub-block's own output
// stream is one-shot, so this module buffers both into local RAM and
// replays them. Validated this buffering/indexing logic (not the GF math,
// already proven) against tests/rs_decoder_ref.py's own rs_decode()
// function via an orchestration-level Python model (2000 trials, including
// deliberately-uncorrectable >16-error cases) before writing this RTL.
//
// Corrections are collected into (position, magnitude) pairs across ALL
// found roots FIRST, and applied to the receive buffer only once every root
// has been evaluated with no Forney failure -- matching rs_decode_ref.py's
// own all-or-nothing semantics (a single bad root aborts the whole
// correction set there too, never touching the output array at all) rather
// than applying corrections incrementally and leaving a partially-corrected
// buffer behind on failure.
//
// EVERY handshake signal feeding or draining a sub-block (s_valid/s_data/
// s_last going in, m_ready coming back) is a pure COMBINATIONAL function of
// `state`/`idx`/the buffers -- never registered. A first draft registered
// them (`synd_s_data <= s_data;` etc, one cycle of latency between
// "decided to feed" and "actually presented"), which desynchronized the
// index counter from what sub-blocks actually received under the
// testbench's randomized backpressure (an off-by-one that silently dropped
// or misaligned bytes, caught by tests/tb_rs_decoder.v -- the SAME class of
// bug the project's existing testbench race-condition note warns about:
// never trust a registered proxy of a handshake signal to line up with the
// same-cycle condition it's being checked against).
module rs_decoder (
    input  wire        clk,
    input  wire        resetn,

    input  wire [7:0]  s_data,
    input  wire        s_valid,
    output wire        s_ready,

    output reg  [7:0]  m_data,
    output reg         m_valid,
    input  wire        m_ready,
    output reg         m_last,

    output reg         fail,          // valid alongside m_last; uncorrectable
                                       // codeword -- m_data carries the raw,
                                       // UNCORRECTED received bytes in that case
    output reg  [5:0]  error_count    // bytes corrected this codeword (0 if
                                       // clean or failed); valid alongside m_last
);
    localparam K = 223;
    localparam NPAR = 32;

    localparam [3:0]
        ST_RECEIVE        = 4'd0,
        ST_DRAIN_SYND     = 4'd1,
        ST_FEED_BM        = 4'd2,
        ST_DRAIN_BM       = 4'd3,
        ST_CHECK_DEGREE   = 4'd4,
        ST_FEED_CHIEN     = 4'd5,
        ST_DRAIN_CHIEN    = 4'd6,
        ST_CHECK_ROOTS    = 4'd7,
        ST_FEED_FORNEY_C  = 4'd8,
        ST_FEED_FORNEY_S  = 4'd9,
        ST_FORNEY_REQ     = 4'd10,
        ST_FORNEY_WAIT    = 4'd11,
        ST_APPLY          = 4'd12,
        ST_OUTPUT         = 4'd13;
    reg [3:0] state;

    reg [8:0] idx;   // shared index: 0..254 (rx_buf/output), 0..31 (synd), 0..32 (coef)
    reg [4:0] ridx;  // 0..15 during the per-root Forney request loop

    reg [5:0] degree_reg;
    reg [5:0] root_count;
    reg       any_forney_fail;

    // ---- Local buffers --------------------------------------------------
    reg [7:0] rx_buf   [0:254];
    reg [7:0] synd_buf [0:31];
    reg [7:0] coef_buf [0:32];
    reg [7:0] root_buf [0:15];
    reg [7:0] mag_buf  [0:15];

    // ---- Per-phase combinational enables ---------------------------------
    wire feed_recv  = (state == ST_RECEIVE);
    wire feed_bm    = (state == ST_FEED_BM);
    wire feed_chien = (state == ST_FEED_CHIEN);
    wire feed_fc    = (state == ST_FEED_FORNEY_C);
    wire feed_fs    = (state == ST_FEED_FORNEY_S);
    wire req_fr     = (state == ST_FORNEY_REQ);

    // ---- Sub-block instances, all handshake ports driven combinationally -
    wire [7:0] synd_m_data;
    wire       synd_s_ready, synd_m_valid, synd_m_last, synd_all_zero;
    wire [7:0] synd_s_data  = s_data;
    wire       synd_s_valid = feed_recv && s_valid;

    wire [7:0] bm_m_data;
    wire       bm_s_ready, bm_m_valid, bm_m_last, bm_done;
    wire [5:0] bm_degree;
    wire [7:0] bm_s_data  = synd_buf[idx];
    wire       bm_s_valid = feed_bm;

    wire [7:0] chien_m_data;
    wire       chien_s_ready, chien_m_valid, chien_search_done;
    wire [7:0] chien_s_data  = coef_buf[idx];
    wire       chien_s_valid = feed_chien;
    wire       chien_s_last  = feed_chien && (idx == {3'b0, degree_reg});

    // Every sub-block's m_ready is held permanently high, not gated by
    // state. This module's OWN case-statement logic already only acts on
    // a block's m_valid/m_data within that block's specific drain state, so
    // gating m_ready too was redundant -- and for TWO of these four blocks
    // it was actively harmful. rs_berlekamp_massey.v only ever clears a
    // stale m_valid (left over from the previous codeword's last drained
    // byte) as a side effect of fully accepting the NEXT codeword's 32
    // syndrome bytes (dcount==NPAR-1) -- it has no out_free-based fallback
    // at all. rs_chien_search.v DOES have such a fallback, but only in its
    // OWN next (`in_data`) phase, which needs an out_free cycle to run it.
    // Gating m_ready to only the exact drain state (the obvious first
    // design) starves both of that: BM can never even begin accepting the
    // next block's first syndrome byte (out_free depends on m_ready since
    // m_valid is stuck, and accept_data depends on out_free -- a genuine
    // deadlock), and Chien has the same latent risk whenever its last
    // tested position (254) happens to be a root. A first attempt gave each
    // one extra state's worth of m_ready (CHECK_DEGREE / CHECK_ROOTS)
    // assuming both worked like Chien's fallback -- that fixed nothing for
    // BM, because BM has no such fallback to feed; only discovered by
    // tracing rs_berlekamp_massey.v's actual RTL line by line after the
    // "one extra cycle" fix measurably failed to unblock it in simulation.
    // Holding m_ready high everywhere sidesteps needing to match each sub-
    // block's own idiosyncratic clearing mechanism at all.
    wire synd_m_ready   = 1'b1;
    wire bm_m_ready     = 1'b1;
    wire chien_m_ready  = 1'b1;

    wire       forney_cs_ready, forney_d_ready, forney_m_valid, forney_fail;
    wire [7:0] forney_pos_data, forney_mag_data;
    wire [7:0] forney_cs_data  = feed_fc ? coef_buf[idx] : synd_buf[idx];
    wire       forney_cs_valid = feed_fc || feed_fs;
    wire       forney_c_last   = feed_fc && (idx == {3'b0, degree_reg});
    wire       forney_cs_last  = feed_fs && (idx == 9'd31);
    wire [7:0] forney_d_data   = root_buf[ridx];
    wire       forney_d_valid  = req_fr;
    wire       forney_m_ready  = 1'b1;

    rs_syndrome u_synd (
        .clk(clk), .resetn(resetn),
        .s_data(synd_s_data), .s_valid(synd_s_valid), .s_ready(synd_s_ready),
        .m_data(synd_m_data), .m_valid(synd_m_valid), .m_ready(synd_m_ready),
        .m_last(synd_m_last), .all_zero(synd_all_zero)
    );
    rs_berlekamp_massey u_bm (
        .clk(clk), .resetn(resetn),
        .s_data(bm_s_data), .s_valid(bm_s_valid), .s_ready(bm_s_ready),
        .m_data(bm_m_data), .m_valid(bm_m_valid), .m_ready(bm_m_ready),
        .m_last(bm_m_last), .degree(bm_degree), .done(bm_done)
    );
    rs_chien_search u_chien (
        .clk(clk), .resetn(resetn),
        .s_data(chien_s_data), .s_valid(chien_s_valid), .s_ready(chien_s_ready),
        .s_last(chien_s_last),
        .m_data(chien_m_data), .m_valid(chien_m_valid), .m_ready(chien_m_ready),
        .search_done(chien_search_done)
    );
    rs_forney u_forney (
        .clk(clk), .resetn(resetn),
        .cs_data(forney_cs_data), .cs_valid(forney_cs_valid), .cs_ready(forney_cs_ready),
        .c_last(forney_c_last), .cs_last(forney_cs_last),
        .d_data(forney_d_data), .d_valid(forney_d_valid), .d_ready(forney_d_ready),
        .pos_data(forney_pos_data), .mag_data(forney_mag_data),
        .m_valid(forney_m_valid), .m_ready(forney_m_ready),
        .forney_fail(forney_fail)
    );

    assign s_ready = feed_recv && synd_s_ready;

    always @(posedge clk) begin
        if (!resetn) begin
            state <= ST_RECEIVE;
            idx <= 9'd0; ridx <= 5'd0;
            degree_reg <= 6'd0; root_count <= 6'd0; any_forney_fail <= 1'b0;
            fail <= 1'b0; error_count <= 6'd0;
            m_valid <= 1'b0; m_last <= 1'b0; m_data <= 8'h00;
        end else begin
            if (m_valid && m_ready) m_valid <= 1'b0;

            case (state)
                ST_RECEIVE: begin
                    // Cleared here, every cycle of the NEXT codeword's
                    // receive phase, rather than in the SAME edge ST_OUTPUT
                    // consumes its final byte -- that edge is also the one a
                    // consumer samples fail/error_count on (alongside
                    // m_last), and clearing them there too made the two
                    // updates land on the exact same ambiguous instant
                    // (caught by tests/tb_rs_decoder.v reporting
                    // error_count=0 on an otherwise byte-correct, non-failed
                    // block -- the value WAS being set correctly, just
                    // already reset again before anything could read it).
                    fail <= 1'b0; error_count <= 6'd0;
                    if (synd_s_valid && synd_s_ready) begin
                        rx_buf[idx] <= s_data;
                        if (idx == 9'd254) begin
                            state <= ST_DRAIN_SYND; idx <= 9'd0;
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_DRAIN_SYND: begin
                    if (synd_m_valid && synd_m_ready) begin
                        synd_buf[idx] <= synd_m_data;
                        if (synd_m_last) begin
                            if (synd_all_zero) begin
                                state <= ST_OUTPUT; idx <= 9'd0;
                            end else begin
                                state <= ST_FEED_BM; idx <= 9'd0;
                            end
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_FEED_BM: begin
                    if (bm_s_valid && bm_s_ready) begin
                        if (idx == 9'd31) begin
                            state <= ST_DRAIN_BM; idx <= 9'd0;
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_DRAIN_BM: begin
                    if (bm_m_valid && bm_m_ready) begin
                        coef_buf[idx] <= bm_m_data;
                        if (idx == 9'd0) degree_reg <= bm_degree;
                        if (bm_m_last) begin
                            state <= ST_CHECK_DEGREE; idx <= 9'd0;
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_CHECK_DEGREE: begin
                    if (degree_reg == 6'd0 || degree_reg > 6'd16) begin
                        fail <= 1'b1;
                        state <= ST_OUTPUT; idx <= 9'd0;
                    end else begin
                        state <= ST_FEED_CHIEN; idx <= 9'd0;
                    end
                end

                ST_FEED_CHIEN: begin
                    if (chien_s_valid && chien_s_ready) begin
                        if (idx == {3'b0, degree_reg}) begin
                            state <= ST_DRAIN_CHIEN; root_count <= 6'd0;
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_DRAIN_CHIEN: begin
                    if (chien_m_valid && chien_m_ready) begin
                        root_buf[root_count[3:0]] <= chien_m_data;
                        root_count <= root_count + 6'd1;
                    end
                    if (chien_search_done) state <= ST_CHECK_ROOTS;
                end

                ST_CHECK_ROOTS: begin
                    if (root_count != degree_reg) begin
                        fail <= 1'b1;
                        state <= ST_OUTPUT; idx <= 9'd0;
                    end else begin
                        state <= ST_FEED_FORNEY_C; idx <= 9'd0;
                    end
                end

                ST_FEED_FORNEY_C: begin
                    if (forney_cs_valid && forney_cs_ready) begin
                        if (idx == {3'b0, degree_reg}) begin
                            state <= ST_FEED_FORNEY_S; idx <= 9'd0;
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_FEED_FORNEY_S: begin
                    if (forney_cs_valid && forney_cs_ready) begin
                        if (idx == 9'd31) begin
                            state <= ST_FORNEY_REQ; ridx <= 5'd0; any_forney_fail <= 1'b0;
                        end else idx <= idx + 9'd1;
                    end
                end

                ST_FORNEY_REQ: begin
                    if (forney_d_valid && forney_d_ready) state <= ST_FORNEY_WAIT;
                end

                ST_FORNEY_WAIT: begin
                    if (forney_m_valid && forney_m_ready) begin
                        mag_buf[ridx] <= forney_mag_data;
                        if (forney_fail) any_forney_fail <= 1'b1;
                        if (ridx == degree_reg[4:0] - 5'd1) begin
                            state <= ST_APPLY; ridx <= 5'd0;
                        end else begin
                            ridx <= ridx + 5'd1;
                            state <= ST_FORNEY_REQ;
                        end
                    end
                end

                // One correction applied per cycle (reusing ridx, now
                // counting 0..degree_reg-1 a SECOND time for this phase),
                // not all of them in one combinational cycle. A first draft
                // used a `for` loop writing up to 16 DIFFERENT runtime-
                // computed rx_buf addresses in a single cycle -- correct in
                // simulation, but synthesis has to build 16 independent
                // write-port decoders into a 255-entry array to support
                // that (since the addresses are only known at runtime, not
                // compile time), which is exactly what OOC synthesis showed:
                // 96% of the part's LUTs and a badly failing 100 MHz timing
                // (WNS -2.25 ns) for what should be a modest design. This
                // correction step is a once-per-codeword control operation,
                // not a throughput-critical path -- there is no reason to
                // pay for 16 parallel write ports when one, reused across a
                // few extra cycles (identical to how RECEIVE/OUTPUT already
                // do one runtime-indexed rx_buf access per cycle without
                // issue), does the exact same job.
                ST_APPLY: begin
                    if (any_forney_fail) begin
                        fail <= 1'b1;
                        state <= ST_OUTPUT; idx <= 9'd0;
                    end else begin
                        rx_buf[254 - root_buf[ridx]] <= rx_buf[254 - root_buf[ridx]] ^ mag_buf[ridx];
                        if (ridx == degree_reg[4:0] - 5'd1) begin
                            error_count <= degree_reg;
                            state <= ST_OUTPUT; idx <= 9'd0;
                        end else begin
                            ridx <= ridx + 5'd1;
                        end
                    end
                end

                ST_OUTPUT: begin
                    // Presenting byte 222 (m_last<=1) and actually being
                    // DONE are two separate events, deliberately not
                    // coupled: a first draft transitioned state<=ST_RECEIVE
                    // in the SAME cycle byte 222 was first PRESENTED (idx
                    // reaching 222), regardless of whether the consumer's
                    // m_ready was high that cycle. Under backpressure that
                    // byte can sit pending for several more cycles -- during
                    // every one of them, state had ALREADY become
                    // ST_RECEIVE, whose own body clears fail/error_count
                    // unconditionally every cycle, wiping them out before
                    // the consumer ever got to read them alongside the
                    // m_last it was still waiting to see asserted with
                    // m_ready. Caught by tests/tb_rs_decoder.v intermittently
                    // (only when backpressure happened to stall exactly on
                    // the final byte), not on every run -- so the FIRST
                    // fix (moving the clear out of this state into
                    // ST_RECEIVE) looked sufficient until backpressure
                    // timing exposed this second, independent gap. The
                    // actual fix: only transition once byte 222 is
                    // genuinely CONSUMED (m_valid && m_ready && m_last, all
                    // reflecting what was already presented, not this
                    // cycle's new presentation).
                    if (m_valid && m_ready && m_last) begin
                        // Final byte just consumed -- move on. Checked
                        // FIRST and made mutually exclusive with the
                        // present-next-byte branch below: once idx stops
                        // advancing past 222 (so this exact byte can sit
                        // pending under backpressure without being
                        // overwritten), the old "present whenever free"
                        // condition would otherwise ALSO fire this same
                        // cycle and re-present rx_buf[222] a second time.
                        state <= ST_RECEIVE; idx <= 9'd0;
                    end else if (!m_valid || m_ready) begin
                        m_data  <= rx_buf[idx];
                        m_valid <= 1'b1;
                        m_last  <= (idx == 9'd222);
                        if (idx != 9'd222) idx <= idx + 9'd1;
                    end
                end

                default: state <= ST_RECEIVE;
            endcase
        end
    end
endmodule
