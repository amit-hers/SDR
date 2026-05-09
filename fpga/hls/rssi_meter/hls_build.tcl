set project_name "rssi_meter"
set part         "xc7z010clg225-1"
set clock_ns     5

open_project   $project_name
set_top        rssi_meter_top
add_files      rssi_meter.cpp
open_solution  "solution1"
set_part       $part
create_clock   -period $clock_ns -name default
config_interface -trim_dangling_ports
csynth_design
export_design -format ip_catalog \
    -description "RSSI Power Accumulator (I^2+Q^2)" \
    -vendor "sdr-link" -library "dsp" -version "1.0"
puts "rssi_meter IP → ./${project_name}/solution1/impl/ip/"
