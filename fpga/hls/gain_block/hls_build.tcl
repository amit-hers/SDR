# gain_block -- Vitis HLS build for the Zynq-7000 in the PlutoSDR.
#
# Run with (2026.1 retired the standalone `vivado_hls` command):
#
#     fpga/hls/build.sh gain_block
#
# or directly:
#
#     source <install>/Vitis/settings64.sh
#     vitis-run --mode hls --tcl fpga/hls/gain_block/hls_build.tcl

source [file join [file dirname [info script]] .. common.tcl]

set project_name "gain_block"
open_project -reset $project_name
add_files gain_block.cpp

set_top gain_block_top
sdr_solution "solution1"
csynth_design
sdr_export "Digital gain / PA gate" "1.0"

puts "gain_block/gain_block_top IP -> ./${project_name}/solution1/impl/ip/"
