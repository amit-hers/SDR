# Out-of-context synthesis check for the HARDWARE variant of the AXI-Lite
# test harness (encoder + syndrome only, no BM) at the real 100MHz AXI-Lite
# clock this project actually uses (PS7 FCLK_CLK0). NOT a full bitstream build.
read_verilog [list \
    [file join [file dirname [info script]] .. fpga rtl rs_encoder.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v] \
    [file join [file dirname [info script]] .. fpga rtl axi_rs_test_harness_hw.v] \
]
synth_design -top axi_rs_test_harness_hw -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports s_axi_aclk]
report_utilization -file [file join [file dirname [info script]] axi_rs_harness_hw_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] axi_rs_harness_hw_timing.rpt]
close_design
