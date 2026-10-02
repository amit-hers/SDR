# Out-of-context synthesis check for the integrated rs_decoder.v (chains
# rs_syndrome -> rs_berlekamp_massey -> rs_chien_search -> rs_forney
# automatically in hardware) at the real 100MHz AXI-Lite clock. NOT a full
# bitstream build.
read_verilog [list \
    [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_berlekamp_massey.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_chien_search.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_forney.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_decoder.v] \
]
synth_design -top rs_decoder -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] rs_decoder_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] rs_decoder_timing.rpt]
close_design
