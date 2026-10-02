# Out-of-context synthesis check for the pipelined qam64_demapper.v alone, at
# the real 100MHz AXI-Lite clock this project's peripherals actually use
# (not the 200MHz HLS-modem-convention check in qam64_ooc_synth.tcl).
# Confirms the 2026-10-02 pipelining fix actually closes the gap found via
# axi_qam64_test_harness_hw.v (WNS -0.396 ns before this fix).
read_verilog [file join [file dirname [info script]] .. fpga rtl qam64_demapper.v]
synth_design -top qam64_demapper -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] qam64_demapper_100mhz_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] qam64_demapper_100mhz_timing.rpt]
close_design
