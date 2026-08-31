# The modem domain is reached only through proper clock-domain crossings:
# axis_clock_converter on both IQ streams, axi_dmac's own store-and-forward on
# the byte streams, and the interconnect's per-master-port clock conversion for
# AXI-Lite. Timing those as synchronous is meaningless -- Vivado picks the worst
# coincidental edge alignment between two unrelated rates and demands the
# impossible:
#
#   Requirement: 0.242ns (clk_out1_..._modem_clk_wiz_0 rise@15733.332
#                         - rx_clk rise@15733.091)
#   Source: axi_tdd_0/...(rx_clk)  Dest: axi_ad9361_dac_dma/i_store_and_forward
#   -> WNS -4.860, 45 failing endpoints, every INTRA-clock domain clean
#
# IMPORTANT: this file must be used_in_implementation only (set in
# system_project.tcl). Applied during synthesis the clocks do not exist yet and
# every constraint silently no-ops -- exactly what happened to the first
# attempt, which logged
#   set_clock_groups:No valid object(s) found for '-group [get_clocks ...]'
# and still "passed", because the modem clock was then an independent PS FCLK
# that Vivado related to nothing. Generating it from clk_fpga_0 with an MMCM
# made the relationship explicit and the missing constraint finally bit.
set_clock_groups -asynchronous \
  -group [get_clocks -quiet clk_out1_system_modem_clk_wiz_0] \
  -group [get_clocks -quiet rx_clk]
set_clock_groups -asynchronous \
  -group [get_clocks -quiet clk_out1_system_modem_clk_wiz_0] \
  -group [get_clocks -quiet clk_fpga_0]
