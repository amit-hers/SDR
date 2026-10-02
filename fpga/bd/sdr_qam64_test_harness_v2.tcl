# sdr_qam64_test_harness_v2.tcl -- AXI-Lite test harness for the 64-QAM
# mapper/demapper (qam64_mapper.v, qam64_demapper.v, with the pipelined
# demapper fix), as a FOURTH peripheral in the SAME bitstream as the RS
# decoder harnesses and the 16-QAM harness (sdr_qam16_test_harness_v2.tcl at
# 0x43C80000). Lets a host script chain RS FEC with 64-QAM modulation the
# same way fpga/probe/rs_qam64_chain_test.py does for 16-QAM. Not part of
# any production signal path.
#
# Needs a DIFFERENT address than the original sdr_qam64_test_harness.tcl
# (0x43C60000, used in the standalone qam64_harness project) -- hence the
# "_v2" name and a new address, same pattern as sdr_qam16_test_harness_v2.tcl.
#
# Source AFTER sdr_version_id.tcl, sdr_rs_test_harness.tcl,
# sdr_rs_decoder_test_harness.tcl, and sdr_qam16_test_harness_v2.tcl.
puts "sdr_qam64_test_harness_v2: AXI-Lite 64-QAM test harness at 0x43C90000"

create_bd_cell -type module -reference axi_qam64_test_harness_hw qam64_test_harness

ad_connect sys_ps7/FCLK_CLK0 qam64_test_harness/s_axi_aclk
ad_connect sys_rstgen/peripheral_aresetn qam64_test_harness/s_axi_aresetn

# 0x43C90000: the next free window after the 16-QAM harness at 0x43C80000
# (itself after the integrated RS decoder harness at 0x43C70000, itself
# after the per-block RS harness at 0x43C60000, itself after the identity
# block at 0x43C50000).
ad_cpu_interconnect 0x43C90000 qam64_test_harness

puts "sdr_qam64_test_harness_v2: mapper+demapper harness wired at 0x43C90000"
