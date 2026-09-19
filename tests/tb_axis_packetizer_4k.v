`timescale 1ns/1ps

module tb_axis_packetizer_4k;
    reg clk = 0;
    always #5 clk = ~clk;

    reg resetn = 0;
    reg [7:0] s_data = 0;
    reg s_valid = 0;
    reg s_last = 0;
    wire s_ready;
    wire [7:0] m_data;
    wire m_valid;
    reg m_ready = 1;
    wire m_keep;
    wire m_last;

    axis_packetizer #(.PKT_BYTES(4096)) dut (
        .clk(clk), .resetn(resetn),
        .s_axis_tdata(s_data), .s_axis_tvalid(s_valid),
        .s_axis_tready(s_ready), .s_axis_tlast(s_last),
        .m_axis_tdata(m_data), .m_axis_tvalid(m_valid),
        .m_axis_tready(m_ready), .m_axis_tkeep(m_keep),
        .m_axis_tlast(m_last)
    );

    integer accepted = 0;
    integer boundaries = 0;
    integer since_last = 0;
    integer first_boundary_accepted = -1;
    integer cycles = 0;

    always @(posedge clk) begin
        cycles <= cycles + 1;
        if (m_valid && m_ready) begin
            if (m_data !== s_data || !m_keep) $fatal(1, "pass-through data/keep mismatch");
            accepted <= accepted + 1;
            since_last <= since_last + 1;
            if (m_last) begin
                boundaries <= boundaries + 1;
                if (boundaries == 0)
                    first_boundary_accepted <= accepted;
                else if (since_last != 4095)
                    $fatal(1, "generated TLAST after %0d beats, expected 4096",
                           since_last + 1);
                since_last <= 0;
            end
        end
        if (cycles > 20000) $fatal(1, "timeout");
    end

    initial begin
        repeat (4) @(posedge clk);
        resetn = 1;
        s_valid = 1;

        // Backpressure must pause the byte counter, not move the boundary.
        repeat (1000) begin
            @(negedge clk);
            m_ready = ((cycles % 7) != 0);
            s_data = s_data + 1;
        end
        m_ready = 1;

        // An upstream TLAST resets the generated-boundary counter.
        @(negedge clk); s_last = 1;
        @(negedge clk); s_last = 0;

        // The next generated TLAST must occur exactly 4096 accepted beats later.
        while (boundaries < 2) begin
            @(negedge clk);
            s_data = s_data + 1;
        end
        if (since_last != 0 || first_boundary_accepted < 0)
            $fatal(1, "counter did not reset at generated TLAST");
        $display("axis_packetizer PKT_BYTES=4096: PASS first=%0d accepted=%0d",
                 first_boundary_accepted, accepted);
        $finish;
    end
endmodule
