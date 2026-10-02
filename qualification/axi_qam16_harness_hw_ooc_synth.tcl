# Out-of-context synthesis check for the 16-QAM hardware harness (mapper +
# demapper) at the real 100MHz AXI-Lite clock. NOT a full bitstream build.
read_verilog [list \
    [file join [file dirname [info script]] .. fpga rtl qam16_mapper.v] \
    [file join [file dirname [info script]] .. fpga rtl qam16_demapper.v] \
    [file join [file dirname [info script]] .. fpga rtl axi_qam16_test_harness_hw.v] \
]
synth_design -top axi_qam16_test_harness_hw -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports s_axi_aclk]
report_utilization -file [file join [file dirname [info script]] axi_qam16_harness_hw_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] axi_qam16_harness_hw_timing.rpt]
close_design
