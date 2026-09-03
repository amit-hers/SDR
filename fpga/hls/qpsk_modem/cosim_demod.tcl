# C/RTL co-simulation for qpsk_demod_top.
#
# csim runs the C model; it CANNOT see faults where synthesised static state
# fails to carry between pipelined transactions. This project has already been
# bitten by exactly that in qpsk_mod (csim bit-exact, cosim correlation 0.551).
# The demodulator is full of static state -- RRC history, the timing NCO's
# hi[]/hq[] window, the Costas accumulators -- pipelined at II=2, and it now
# demonstrably behaves differently on hardware than in csim on the SAME input:
# csim recovers 99 bytes from a captured hardware signal at +18.2 dB SNR while
# the FPGA scores 2-4. Cosim is the only tool that can see the difference.
#
# One vector only: RTL simulation of 4096 samples through this pipeline is slow.
source [file join [file dirname [info script]] .. common.tcl]
set vec_dir [file normalize [file join [file dirname [info script]] .. vectors]]

open_project -reset qpsk_demod_cosim
add_files qpsk_demod.cpp
add_files -tb qpsk_demod_tb.cpp -cflags "-I../vectors"
set_top qpsk_demod_top
sdr_solution "solution1"
csynth_design
cosim_design -argv "$vec_dir clean" -trace_level none
