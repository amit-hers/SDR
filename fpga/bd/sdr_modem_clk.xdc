# Modem clock domain constraints.
#
# ADI disables the PS7-generated XDC (system_project.tcl:
#   set_property is_enabled false [get_files *system_sys_ps7_0.xdc])
# and declares each FCLK by hand in system_constr.xdc -- but only FCLKCLK[0]
# and [1]. sdr_insert.tcl adds FCLKCLK[2] for the modem, so without the
# create_clock below that entire domain is UNCONSTRAINED.
#
# That failure is silent and actively misleading: the run reports "timing met"
# with zero failing endpoints because the modem's paths were never analysed.
# The only symptom was
#   CRITICAL WARNING: There are 11059 registers with no clocks !!!
# and the absence of clk_fpga_2 from the clock summary. Timing reports are
# only as trustworthy as the clock list they were computed against.
# 32 ns (31.25 MHz), deliberately tighter than the 30 MHz requested of the PS.
# Two reasons. The PS PLL cannot hit 30.000 MHz exactly -- its integer dividers
# land near 30.3 MHz (33.0 ns) -- and since ADI disables the PS7's own XDC,
# THIS create_clock is the only definition the tools have. Constraining
# slightly faster than the hardware can actually run is the safe direction; the
# reverse silently under-verifies.
#
# 40 MHz (25 ns) was tried first, as specified, and missed: WNS -2.711 ns over
# 24 endpoints, i.e. the routed path needs ~27.7 ns. That is consistent with
# the 26.881 ns measured when the modem was still in the rx_clk domain, and
# with HLS's 44.41 MHz estimate being optimistic by the usual pre-route margin.
# 32 ns leaves ~4 ns of slack and is still ~4x the 8 MS/s the link needs.
create_clock -name clk_fpga_2 -period 32.000 \
  [get_pins "i_system_wrapper/system_i/sys_ps7/inst/PS7_i/FCLKCLK[2]"]

# The modem exchanges data with the radio domain only through
# axis_clock_converter instances (rx_iq_cc / tx_iq_cc) and with the CPU domain
# only through the interconnect's per-port clock conversion. Every one of those
# does its own synchronisation, so cross-domain paths must not be timed as if
# they were synchronous -- otherwise the tool reports violations on paths that
# hardware never has to meet.
set_clock_groups -asynchronous \
  -group [get_clocks clk_fpga_2] \
  -group [get_clocks rx_clk]
set_clock_groups -asynchronous \
  -group [get_clocks clk_fpga_2] \
  -group [get_clocks clk_fpga_0]
