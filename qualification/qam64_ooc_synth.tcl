# Out-of-context synthesis check for the QAM64 mapper/demapper RTL alone --
# NOT a full bitstream build, not part of the production project. Purpose:
# confirm the two modules synthesize cleanly on the actual target part
# (resource usage, timing estimate, no synthesis-level errors) as an
# incremental, low-risk step on roadmap item 7 (64-QAM FPGA modem),
# distinct from and much smaller than the real remaining work (integrating
# these blocks into the actual modem pipeline with acquisition/carrier/
# timing recovery, which needs real RTL design work, not just this check).
#
# Clock constraint matches the existing HLS modem cores' own OOC convention
# (fpga/hls/qpsk_modem/*/impl/ip/constraints/*_top_ooc.xdc: ap_clk @ 5.000 ns
# = 200 MHz) so the timing numbers here are comparable to blocks these would
# sit alongside, not just "no constraint, no failure."
read_verilog [glob [file join [file dirname [info script]] .. fpga rtl qam64_*.v]]
synth_design -top qam64_mapper -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] qam64_mapper_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] qam64_mapper_timing.rpt]
close_design

read_verilog [glob [file join [file dirname [info script]] .. fpga rtl qam64_*.v]]
synth_design -top qam64_demapper -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] qam64_demapper_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] qam64_demapper_timing.rpt]
close_design
