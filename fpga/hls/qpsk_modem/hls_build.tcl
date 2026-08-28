# qpsk_modem -- Vitis HLS build for the Zynq-7000 in the PlutoSDR.
#
# Two independent projects, one per core. They must be separate: HLS scopes
# add_files to the project, so a single project cannot hold both testbenches
# (two mains), and the modulator would re-parse the demodulator's heavy math
# headers for nothing.
#
#     fpga/hls/build.sh qpsk_modem

source [file join [file dirname [info script]] .. common.tcl]

set vec_dir [file normalize [file join [file dirname [info script]] .. vectors]]

# SDR_HLS_BYPASS isolates a demod stage during bring-up, e.g.
#   SDR_HLS_BYPASS="-DSDR_BYPASS_TIMING=1 -DSDR_BYPASS_COSTAS=1"
set bypass ""
if {[info exists ::env(SDR_HLS_BYPASS)]} { set bypass $::env(SDR_HLS_BYPASS) }

# ── demodulator ──────────────────────────────────────────────────────────
open_project -reset qpsk_demod
add_files qpsk_demod.cpp -cflags $bypass
add_files -tb qpsk_demod_tb.cpp -cflags "-I../vectors $bypass"
set_top qpsk_demod_top
sdr_solution "solution1"
csim_design -argv $vec_dir
csynth_design
sdr_export "QPSK Demodulator (RRC + AGC + Timing + Costas)" "2.0"

# ── modulator ────────────────────────────────────────────────────────────
open_project -reset qpsk_mod
add_files qpsk_mod.cpp -cflags $bypass
add_files -tb qpsk_mod_tb.cpp -cflags "-I../vectors $bypass"
set_top qpsk_mod_top
sdr_solution "solution1"
csim_design -argv $vec_dir
csynth_design
sdr_export "QPSK/BPSK Modulator (RRC polyphase interpolation)" "2.0"

puts "qpsk_demod IP -> ./qpsk_demod/solution1/impl/ip/"
puts "qpsk_mod   IP -> ./qpsk_mod/solution1/impl/ip/"
