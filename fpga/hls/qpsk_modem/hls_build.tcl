set project_name "qpsk_modem"
set part         "xc7z010clg225-1"
set clock_ns     5

open_project   $project_name

# Both RX demod and TX mod share the same file
add_files      qpsk_modem.cpp

# Build demod first
set_top        qpsk_demod_top
open_solution  "solution_demod"
set_part       $part
create_clock   -period $clock_ns -name default
config_interface -trim_dangling_ports
csynth_design
export_design -format ip_catalog \
    -description "QPSK Demodulator (RRC + AGC + Timing + Costas)" \
    -vendor "sdr-link" -library "dsp" -version "2.0"

# Build mod
set_top        qpsk_mod_top
open_solution  "solution_mod"
set_part       $part
create_clock   -period $clock_ns -name default
config_interface -trim_dangling_ports
csynth_design
export_design -format ip_catalog \
    -description "QPSK/BPSK Modulator (RRC polyphase interpolation)" \
    -vendor "sdr-link" -library "dsp" -version "2.0"

puts "Demod IP → ./${project_name}/solution_demod/impl/ip/"
puts "Mod   IP → ./${project_name}/solution_mod/impl/ip/"
