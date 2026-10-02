# sdr_rs_decoder_test_harness.tcl -- AXI-Lite test harness for the
# INTEGRATED RS(255,223) decoder core (rs_decoder.v: syndrome->BM->Chien->
# Forney chained automatically in hardware), as a SECOND peripheral
# alongside the existing per-block harness in this same project
# (sdr_rs_test_harness.tcl, at 0x43C60000). Exists ONLY to exercise this
# core on real silicon via register polling from the ARM; it is NOT part of
# any production signal path (no connection to the AD9361, no DMA).
#
# Source AFTER sdr_version_id.tcl and sdr_rs_test_harness.tcl.
puts "sdr_rs_decoder_test_harness: AXI-Lite integrated RS decoder test harness at 0x43C70000"

create_bd_cell -type module -reference axi_rs_decoder_test_harness_hw rs_decoder_test_harness

ad_connect sys_ps7/FCLK_CLK0 rs_decoder_test_harness/s_axi_aclk
ad_connect sys_rstgen/peripheral_aresetn rs_decoder_test_harness/s_axi_aresetn

# 0x43C70000: the next free window after the per-block harness at
# 0x43C60000 (itself the next free window after the identity block at
# 0x43C50000).
ad_cpu_interconnect 0x43C70000 rs_decoder_test_harness

puts "sdr_rs_decoder_test_harness: integrated decoder harness wired at 0x43C70000"
