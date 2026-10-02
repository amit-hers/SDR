# sdr_qam16_test_harness.tcl -- AXI-Lite test harness for the 16-QAM
# mapper/demapper (qam16_mapper.v, qam16_demapper.v). Exists ONLY to exercise
# those blocks on real silicon via register polling from the ARM; it is NOT
# part of any production signal path (no connection to the AD9361, no DMA).
#
# Source AFTER sdr_version_id.tcl, in a project that does NOT source
# sdr_insert.tcl (a separate, modem-free bitstream variant -- see
# adi-hdl/projects/qam16_harness/, not the deployed adi-hdl/projects/libre/).
puts "sdr_qam16_test_harness: AXI-Lite 16-QAM test harness at 0x43C60000"

create_bd_cell -type module -reference axi_qam16_test_harness_hw qam16_test_harness

# Same clock/reset as the identity block: a fixed PS7 peripheral clock, not
# tied to any modem or sample-rate domain that does not exist in this build.
ad_connect sys_ps7/FCLK_CLK0 qam16_test_harness/s_axi_aclk
ad_connect sys_rstgen/peripheral_aresetn qam16_test_harness/s_axi_aresetn

# 0x43C60000: the next free window after the identity block at 0x43C50000.
# (This project never includes the RS harness, so there is no collision with
# rs_harness's own use of this same address in its separate project.)
ad_cpu_interconnect 0x43C60000 qam16_test_harness

puts "sdr_qam16_test_harness: mapper+demapper harness wired at 0x43C60000"
