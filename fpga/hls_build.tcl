# hls_build.tcl  —  Vivado HLS project for QPSK modem IP
#
# Usage:
#   vivado_hls -f hls_build.tcl
#
# Output: ./hls_project/qpsk_modem/impl/ip/ — ready to add to Vivado IP catalog

set project_name "qpsk_modem"
set part         "xc7z010clg225-1"   ;# Zynq-7010 (PlutoSDR / HamGeek Pluto+)
set clock_ns     5                    ;# 200 MHz target clock

open_project   $project_name
set_top        qpsk_demod_top

add_files      qpsk_demod_hls.cpp

open_solution  "solution1"
set_part       $part
create_clock   -period $clock_ns -name default

# Synthesis directives
config_compile -name_max_length 80
config_interface -trim_dangling_ports

# Run C simulation (requires test bench — add qpsk_tb.cpp)
# csim_design -clean

# Synthesize
csynth_design

# Export as IP (AXI4-Stream interface)
export_design -format ip_catalog -description "QPSK Modem AXI4-Stream IP" \
              -vendor "sdr-link" -library "dsp" -version "1.0"

# Report
puts "================================"
puts "Synthesis complete."
puts "IP output: ./${project_name}/solution1/impl/ip/"
puts "Add to Vivado IP catalog, then:"
puts "  1. Open pluto_plus.xpr in Vivado"
puts "  2. IP Integrator → Add IP → qpsk_modem_v1_0"
puts "  3. Connect s_axis_iq to cf-ad9361-lpc M_AXIS_0"
puts "  4. Connect m_axis_bits to AXI DMA S_AXIS_S2MM"
puts "  5. Generate Bitstream → export to SD card"
puts "================================"
