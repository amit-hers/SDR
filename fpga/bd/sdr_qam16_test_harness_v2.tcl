# sdr_qam16_test_harness_v2.tcl -- AXI-Lite test harness for the 16-QAM
# mapper/demapper (qam16_mapper.v, qam16_demapper.v), as a THIRD peripheral
# in the SAME bitstream as the RS decoder harnesses (sdr_rs_test_harness.tcl
# at 0x43C60000, sdr_rs_decoder_test_harness.tcl at 0x43C70000). Exists to
# let a host script exercise RS FEC and 16-QAM modulation TOGETHER -- encode
# with the real RS hardware, map to symbols with the real 16-QAM hardware,
# (optionally corrupt symbols in software, simulating a lossy channel),
# demap back, decode with the real RS hardware -- something neither
# previous harness could do alone. Not part of any production signal path.
#
# Needs a DIFFERENT address than the original sdr_qam16_test_harness.tcl
# (0x43C60000), which collides with the RS per-block harness already using
# that address in THIS project -- hence the "_v2" name and a new address.
#
# Source AFTER sdr_version_id.tcl, sdr_rs_test_harness.tcl, and
# sdr_rs_decoder_test_harness.tcl.
puts "sdr_qam16_test_harness_v2: AXI-Lite 16-QAM test harness at 0x43C80000"

create_bd_cell -type module -reference axi_qam16_test_harness_hw qam16_test_harness

ad_connect sys_ps7/FCLK_CLK0 qam16_test_harness/s_axi_aclk
ad_connect sys_rstgen/peripheral_aresetn qam16_test_harness/s_axi_aresetn

# 0x43C80000: the next free window after the integrated RS decoder harness
# at 0x43C70000 (itself after the per-block RS harness at 0x43C60000, itself
# after the identity block at 0x43C50000).
ad_cpu_interconnect 0x43C80000 qam16_test_harness

puts "sdr_qam16_test_harness_v2: mapper+demapper harness wired at 0x43C80000"
