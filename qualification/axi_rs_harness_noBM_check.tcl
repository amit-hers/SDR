# Scope check: does the encoder+syndrome pair alone (without BM) meet the
# real 100MHz AXI-Lite peripheral clock this project actually uses?
read_verilog [list \
    [file join [file dirname [info script]] .. fpga rtl rs_encoder.v] \
    [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v] \
]
synth_design -top rs_encoder -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports clk]
report_timing_summary -file [file join [file dirname [info script]] rs_encoder_timing_100mhz.rpt]
close_design

read_verilog [file join [file dirname [info script]] .. fpga rtl rs_syndrome.v]
synth_design -top rs_syndrome -part xc7z020clg400-2 -mode out_of_context
create_clock -name clk -period 10.000 [get_ports clk]
report_timing_summary -file [file join [file dirname [info script]] rs_syndrome_timing_100mhz.rpt]
close_design
