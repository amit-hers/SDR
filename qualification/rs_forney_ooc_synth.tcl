# Out-of-context synthesis check for the Forney RTL alone -- NOT a full
# bitstream build. Same convention as the other *_ooc_synth.tcl scripts. This
# completes the RS(255,223) decoder chain: syndromes -> Berlekamp-Massey ->
# Chien search -> Forney (this block) locates AND corrects the errors.
read_verilog [file join [file dirname [info script]] .. fpga rtl rs_forney.v]
synth_design -top rs_forney -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] rs_forney_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] rs_forney_timing.rpt]
close_design
