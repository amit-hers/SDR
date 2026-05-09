set project_name "gain_block"
set part         "xc7z010clg225-1"
set clock_ns     5

open_project   $project_name
set_top        gain_block_top
add_files      gain_block.cpp
open_solution  "solution1"
set_part       $part
create_clock   -period $clock_ns -name default
config_interface -trim_dangling_ports
csynth_design
export_design -format ip_catalog \
    -description "Digital Gain / PA Safety Gate" \
    -vendor "sdr-link" -library "dsp" -version "1.0"
puts "gain_block IP → ./${project_name}/solution1/impl/ip/"
