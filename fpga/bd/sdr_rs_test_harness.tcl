# sdr_rs_test_harness.tcl -- AXI-Lite test harness for the RS(255,223)
# encoder and syndrome calculator built this session. Exists ONLY to exercise
# those blocks on real silicon via register polling from the ARM; it is NOT
# part of any production signal path (no connection to the AD9361, no DMA).
#
# Source AFTER sdr_version_id.tcl, in a project that does NOT source
# sdr_insert.tcl (this is a separate, modem-free bitstream variant -- see
# adi-hdl/projects/rs_harness/, not the deployed adi-hdl/projects/libre/).
puts "sdr_rs_test_harness: AXI-Lite RS test harness at 0x43C60000"

create_bd_cell -type module -reference axi_rs_test_harness_hw rs_test_harness

# Same clock/reset as the identity block: a fixed PS7 peripheral clock, not
# tied to any modem or sample-rate domain that does not exist in this build.
ad_connect sys_ps7/FCLK_CLK0 rs_test_harness/s_axi_aclk
ad_connect sys_rstgen/peripheral_aresetn rs_test_harness/s_axi_aresetn

# 0x43C60000: the next free window after the identity block at 0x43C50000.
ad_cpu_interconnect 0x43C60000 rs_test_harness

puts "sdr_rs_test_harness: encoder+syndrome harness wired at 0x43C60000"
