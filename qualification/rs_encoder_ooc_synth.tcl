# Out-of-context synthesis check for the RS(255,223) encoder RTL alone --
# NOT a full bitstream build. Same purpose and clock convention as
# qam64_ooc_synth.tcl / qam16_ooc_synth.tcl: confirm the module synthesizes
# cleanly and meets timing on the real target part before any integration
# work. This is F09's encoder half only; the decoder (syndromes,
# Berlekamp-Massey, Chien search, Forney) is unstarted.
read_verilog [file join [file dirname [info script]] .. fpga rtl rs_encoder.v]
synth_design -top rs_encoder -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 5.000 [get_ports clk]
report_utilization -file [file join [file dirname [info script]] rs_encoder_utilization.rpt]
report_timing_summary -file [file join [file dirname [info script]] rs_encoder_timing.rpt]
close_design
