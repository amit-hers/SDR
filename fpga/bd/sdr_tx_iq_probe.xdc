# Constraints for the TEMPORARY TX IQ probe (sdr_tx_iq_probe.tcl).
#
# ctrl/stat cross between clk_fpga_0 (the axi_gpio) and rx_clk (the probe's
# l_clk capture logic). The RTL synchronises ctrl with two flops and holds it
# static during readback, so these crossings are false paths -- but they must
# be declared, or Vivado times them and the design fails: the first build put
# the GPIO output registers straight onto the block RAM address port and
# reported WNS -2.856 ns across 23 clk_fpga_0 -> rx_clk endpoints.
#
# Scoped to the probe's own cells on purpose. A blanket
# set_clock_groups -asynchronous between clk_fpga_0 and rx_clk would also
# "fix" it while hiding genuine crossings inside ADI's logic.
set_false_path -to   [get_cells -quiet -hier -regexp {.*tx_iq_probe.*/ctrl_s1_reg.*}]
set_false_path -to   [get_cells -quiet -hier -regexp {.*rx_iq_probe.*/ctrl_s1_reg.*}]
set_false_path -from [get_cells -quiet -hier -regexp {.*tx_iq_probe.*/(rdata_reg|waddr_reg|running_reg|done_reg).*}]
set_false_path -from [get_cells -quiet -hier -regexp {.*rx_iq_probe.*/(rdata_reg|waddr_reg|running_reg|done_reg).*}]

# dac_pin_probe: same cross-domain structure as the two IQ probes.
set_false_path -to   [get_cells -quiet -hier -regexp {.*dac_probe.*/ctrl_s1_reg.*}]
set_false_path -from [get_cells -quiet -hier -regexp {.*dac_probe.*/(rdata_reg|waddr_reg|running_reg|done_reg).*}]
