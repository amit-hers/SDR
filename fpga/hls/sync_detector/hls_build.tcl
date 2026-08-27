# sync_detector -- Vitis HLS build for the Zynq-7000 in the PlutoSDR.
#
# Run with (2026.1 retired the standalone `vivado_hls` command):
#
#     fpga/hls/build.sh sync_detector
#
# or directly:
#
#     source <install>/Vitis/settings64.sh
#     vitis-run --mode hls --tcl fpga/hls/sync_detector/hls_build.tcl

source [file join [file dirname [info script]] .. common.tcl]

set project_name "sync_detector"
open_project -reset $project_name
add_files sync_detector.cpp

set_top sync_detector_top
sdr_solution "solution1"
csynth_design
sdr_export "Frame sync-word detector" "1.0"

puts "sync_detector/sync_detector_top IP -> ./${project_name}/solution1/impl/ip/"
