# Out-of-context synthesis check for the Chien search RTL alone -- NOT a full
# bitstream build. Same convention as the other *_ooc_synth.tcl scripts. This
# is the error-location stage only; Forney (computing error MAGNITUDES, i.e.
# actually correcting the errors) is not implemented.
read_verilog [file join [file dirname [info script]] .. fpga rtl rs_chien_search.v]
synth_design -top rs_chien_search -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] rs_chien_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] rs_chien_timing.rpt]
close_design
