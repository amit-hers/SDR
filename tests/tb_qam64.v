`timescale 1ns/1ps
module tb_qam64;
    reg clk=0; always #5 clk=~clk;
    reg resetn=0, valid=0, ready=1;
    reg [5:0] data=0;
    wire sr, mv, dr, dv;
    wire signed [15:0] mi,mq;
    wire [5:0] decoded;
    wire [31:0] error;
    wire clipped;
    reg inject=0;
    reg signed [15:0] test_i=0,test_q=0;
    qam64_mapper mapper(clk,resetn,data,valid,sr,mi,mq,mv,dr);
    qam64_demapper demapper(clk,resetn,inject?test_i:mi,inject?test_q:mq,
                           inject?valid:mv,dr,decoded,error,clipped,dv,ready);
    integer i,j,received=0;
    integer expected_q[$];
    reg [5:0] held;
    reg [31:0] vectors[0:63];
    reg [4095:0] vector_file;
    integer have_vectors, delta_i, delta_q;
    function integer gray;
        input integer b;
        begin gray=b^(b>>1); end
    endfunction
    // qam64_demapper is now a 2-stage pipeline (pipelined to meet the real
    // 100 MHz AXI-Lite clock -- see its own header), so its latency is no
    // longer 1 cycle. Rather than hand-derive and hardcode the new total
    // mapper+demapper latency here, results are drained through a FIFO
    // queue in arrival order -- correct for any fixed pipeline depth as
    // long as throughput stays at one symbol in, one result out, in order.
    task check_axis;
        input integer x;
        input integer b;
        begin
            @(negedge clk); inject=1;valid=1;test_i=x;test_q=1264;
            while (!dv) @(posedge clk);
            #1;
            if (decoded !== ((gray(b)<<3)|gray(4))) $fatal(1,"boundary %d result %d",x,decoded);
            @(negedge clk); valid=0;
            while (dv) @(posedge clk); // drain before the next injected case
        end
    endtask
    initial begin
        have_vectors=$value$plusargs("vectors=%s",vector_file);
        if(have_vectors) $readmemh(vector_file,vectors);
        repeat(3) @(negedge clk); resetn=1;
        // All points against an independent Gray/index expectation.
        for(i=0;i<64;i=i+1) begin
            @(negedge clk); data=i;valid=1;
            expected_q.push_back(i);
            @(posedge clk); #1;
            if(have_vectors) begin
                delta_i=$signed(mi)-$signed(vectors[i][31:16]);
                delta_q=$signed(mq)-$signed(vectors[i][15:0]);
                if(delta_i < -1 || delta_i > 1 || delta_q < -1 || delta_q > 1)
                    $fatal(1,"software/RTL mapping mismatch at %d",i);
            end
            if (dv) begin
                if(decoded!==expected_q.pop_front() || error!==0) $fatal(1,"point mismatch pushing %0d",i);
                received=received+1;
            end
        end
        @(negedge clk);valid=0;
        // Drain whatever is still in flight -- however many cycles that
        // takes for the current pipeline depth.
        while (expected_q.size() > 0) begin
            @(posedge clk); #1;
            if (!dv) $fatal(1,"pipeline stopped draining with %0d results still expected", expected_q.size());
            if(decoded!==expected_q.pop_front() || error!==0) $fatal(1,"drain mismatch");
            received=received+1;
        end
        // Backpressure must hold data/error stable.
        @(negedge clk); ready=0;valid=1;data=17;
        while (!dv) @(posedge clk);
        #1; held=decoded;
        repeat(5) begin
            @(negedge clk); data=data+1;
            @(posedge clk);#1;
            if(decoded!==held || dr!==0) $fatal(1,"backpressure");
        end
        @(negedge clk);ready=1;valid=0;resetn=0;
        @(posedge clk);#1;
        if(dv || mv) $fatal(1,"reset pending valid");
        @(negedge clk);resetn=1;
        for(j=1;j<8;j=j+1) begin
            check_axis((j*2-8)*1264-1,j-1);
            check_axis((j*2-8)*1264,j);
            check_axis((j*2-8)*1264+1,j);
        end
        check_axis(-32768,0);
        if(!clipped) $fatal(1,"negative clipping");
        check_axis(32767,7);
        if(!clipped) $fatal(1,"positive clipping");
        if(received!=64) $fatal(1,"invalid stimulus");
        $display("PASS: 64 symbols, decision boundaries, backpressure, reset, clipping");
        $finish;
    end
    initial begin #100000; $fatal(1,"timeout"); end
endmodule
