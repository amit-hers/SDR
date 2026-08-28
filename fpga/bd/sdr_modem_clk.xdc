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
create_clock -name clk_fpga_2 -period 25.000 \
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
