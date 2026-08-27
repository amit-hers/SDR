# rssi_meter -- Vitis HLS build for the Zynq-7000 in the PlutoSDR.
#
# Run with (2026.1 retired the standalone `vivado_hls` command):
#
#     fpga/hls/build.sh rssi_meter
#
# or directly:
#
#     source <install>/Vitis/settings64.sh
#     vitis-run --mode hls --tcl fpga/hls/rssi_meter/hls_build.tcl

source [file join [file dirname [info script]] .. common.tcl]

set project_name "rssi_meter"
open_project -reset $project_name
add_files rssi_meter.cpp

set_top rssi_meter_top
sdr_solution "solution1"
csynth_design
sdr_export "RSSI Power Accumulator (I^2+Q^2)" "1.0"

puts "rssi_meter/rssi_meter_top IP -> ./${project_name}/solution1/impl/ip/"
