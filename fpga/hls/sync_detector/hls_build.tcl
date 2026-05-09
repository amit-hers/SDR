set project_name "sync_detector"
set part         "xc7z010clg225-1"
set clock_ns     5

open_project   $project_name
set_top        sync_detector_top
add_files      sync_detector.cpp
open_solution  "solution1"
set_part       $part
create_clock   -period $clock_ns -name default
config_interface -trim_dangling_ports
csynth_design
export_design -format ip_catalog \
    -description "Frame Sync Word Correlator (0xC0FFEE77)" \
    -vendor "sdr-link" -library "dsp" -version "1.0"
puts "sync_detector IP → ./${project_name}/solution1/impl/ip/"
