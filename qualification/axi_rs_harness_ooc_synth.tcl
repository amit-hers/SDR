# Out-of-context synthesis check for the AXI-Lite test harness wrapper
# (encoder + syndrome + BM instantiated together) before integrating it into
# the real block design. NOT a full bitstream build.
read_verilog [list \
    [file join [file dirname [info script]] .. fpga rtl rs_encoder.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_berlekamp_massey.v] \
    [file join [file dirname [info script]] .. fpga rtl axi_rs_test_harness.v] \
]
synth_design -top axi_rs_test_harness -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports s_axi_aclk]
report_utilization -file [file join [file dirname [info script]] axi_rs_harness_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] axi_rs_harness_timing.rpt]
close_design
