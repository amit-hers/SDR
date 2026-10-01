# Out-of-context synthesis check for the QAM16 mapper/demapper RTL alone --
# NOT a full bitstream build, not part of the production project. Same
# purpose as qam64_ooc_synth.tcl: confirm the two modules synthesize cleanly
# on the actual target part before any integration work.
#
# Clock constraint matches the existing HLS modem cores' own OOC convention
# (fpga/hls/qpsk_modem/*/impl/ip/constraints/*_top_ooc.xdc: ap_clk @ 5.000 ns
# = 200 MHz) so the timing numbers here are comparable to blocks these would
# sit alongside, not just "no constraint, no failure."
read_verilog [glob [file join [file dirname [info script]] .. fpga rtl qam16_*.v]]
synth_design -top qam16_mapper -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] qam16_mapper_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] qam16_mapper_timing.rpt]
close_design

read_verilog [glob [file join [file dirname [info script]] .. fpga rtl qam16_*.v]]
synth_design -top qam16_demapper -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] qam16_demapper_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] qam16_demapper_timing.rpt]
close_design
