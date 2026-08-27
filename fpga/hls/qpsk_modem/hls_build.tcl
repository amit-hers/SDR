# qpsk_modem -- Vitis HLS build for the Zynq-7000 in the PlutoSDR.
#
# Run with (2026.1 retired the standalone `vivado_hls` command):
#
#     fpga/hls/build.sh qpsk_modem
#
# or directly:
#
#     source <install>/Vitis/settings64.sh
#     vitis-run --mode hls --tcl fpga/hls/qpsk_modem/hls_build.tcl

source [file join [file dirname [info script]] .. common.tcl]

set project_name "qpsk_modem"
open_project -reset $project_name

set_top qpsk_demod_top
# The testbench drives the core with IQ from the validated host TX path and
# requires the transmitted bytes back. Run before synthesis: a core that does
# not compute the right answer is not worth scheduling.
# SDR_HLS_BYPASS lets a bring-up run isolate a stage, e.g.
#   SDR_HLS_BYPASS="-DSDR_BYPASS_TIMING=1 -DSDR_BYPASS_COSTAS=1"
set bypass ""
if {[info exists ::env(SDR_HLS_BYPASS)]} { set bypass $::env(SDR_HLS_BYPASS) }
add_files qpsk_modem.cpp -cflags $bypass
add_files -tb qpsk_demod_tb.cpp -cflags "-I../vectors $bypass"
sdr_solution "solution_demod"
# csim runs several directories below the core, so hand the testbench an
# absolute path rather than a relative one that silently resolves nowhere.
set vec_dir [file normalize [file join [file dirname [info script]] .. vectors]]
csim_design -argv $vec_dir
csynth_design
sdr_export "QPSK Demodulator (RRC + AGC + Timing + Costas)" "2.0"

set_top qpsk_mod_top
sdr_solution "solution_mod"
csynth_design
sdr_export "QPSK/BPSK Modulator (RRC polyphase interpolation)" "2.0"

puts "qpsk_modem/qpsk_demod_top IP -> ./${project_name}/solution_demod/impl/ip/"
puts "qpsk_modem/qpsk_mod_top IP -> ./${project_name}/solution_mod/impl/ip/"
