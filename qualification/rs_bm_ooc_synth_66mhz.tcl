# Out-of-context synthesis check for the Berlekamp-Massey RTL alone -- NOT a
# full bitstream build. Same convention as the other *_ooc_synth.tcl scripts.
# This is the error-locator stage only; Chien search and Forney (locating and
# correcting the actual errors from this polynomial) are not implemented.
read_verilog [file join [file dirname [info script]] .. fpga rtl rs_berlekamp_massey.v]
synth_design -top rs_berlekamp_massey -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 15.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] rs_bm_utilization_66mhz.rpt]
report_timing_summary -file [file join [file dirname [info script]] rs_bm_timing_66mhz.rpt]
close_design
