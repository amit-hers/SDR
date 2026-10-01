# Out-of-context synthesis check for the RS syndrome calculator RTL alone --
# NOT a full bitstream build. Same convention as the other *_ooc_synth.tcl
# scripts in this directory. First decoder stage only; Berlekamp-Massey,
# Chien search and Forney are not implemented.
read_verilog [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v]
synth_design -top rs_syndrome -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] rs_syndrome_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] rs_syndrome_timing.rpt]
close_design
