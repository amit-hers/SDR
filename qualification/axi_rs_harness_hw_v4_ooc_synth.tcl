# Out-of-context synthesis check for the v4 hardware harness (encoder +
# syndrome + Chien search + Forney + Berlekamp-Massey -- all five RS decoder
# blocks) at the real 100MHz AXI-Lite clock. NOT a full bitstream build.
read_verilog [list \
    [file join [file dirname [info script]] .. fpga rtl rs_encoder.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_chien_search.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_forney.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_berlekamp_massey.v] \
    [file join [file dirname [info script]] .. fpga rtl axi_rs_test_harness_hw.v] \
]
synth_design -top axi_rs_test_harness_hw -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports s_axi_aclk]
report_utilization -file [file join [file dirname [info script]] axi_rs_harness_hw_v4_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] axi_rs_harness_hw_v4_timing.rpt]
close_design
