# sdr_tx_iq_probe.tcl -- TEMPORARY. Source AFTER sdr_insert.tcl.
#
# Splices iq_probe into the TX IQ stream immediately before axis_to_adi_iq, so
# the modulator's pulse-shaped output can be read back and compared against the
# host RRC reference. Kept out of sdr_insert.tcl so the verified integration
# stays byte-identical; delete the `source` line to remove it.
#
# The probe is a combinational pass-through, so the TX path behaves exactly as
# it does without it -- which is the point, since dac_dunf must stay clean
# while measuring.
puts "sdr_tx_iq_probe: splicing TX IQ capture before axis_to_iq"

create_bd_cell -type module -reference iq_probe tx_iq_probe

# Re-route tx_iq_cc/M_AXIS -> axis_to_iq/s_axis through the probe.
set n [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins tx_iq_cc/M_AXIS]]
if {[llength $n]} { delete_bd_objs $n }
ad_connect tx_iq_cc/M_AXIS    tx_iq_probe/s_axis
ad_connect tx_iq_probe/m_axis axis_to_iq/s_axis

# The M_AXIS side of the clock converter is in the l_clk domain.
ad_connect axi_ad9361/l_clk tx_iq_probe/clk
ad_connect sdr_rst_inv/Res  tx_iq_probe/resetn

# Readout: ch1 out = ctrl (arm/sel/raddr), ch2 in = stat (sample or count).
ad_ip_instance axi_gpio tx_iq_probe_gpio [list \
  C_IS_DUAL 1 C_ALL_OUTPUTS 1 C_ALL_INPUTS_2 1 \
  C_GPIO_WIDTH 32 C_GPIO2_WIDTH 32]
ad_connect tx_iq_probe_gpio/gpio_io_o  tx_iq_probe/ctrl
ad_connect tx_iq_probe_gpio/gpio2_io_i tx_iq_probe/stat
ad_cpu_interconnect 0x43C20000 tx_iq_probe_gpio

puts "sdr_tx_iq_probe: gpio at 0x43C20000 (ctrl=0x00 data, stat=0x08 data)"

# Timing constraints for the probe's clock-domain crossings.
set _xdc [file join [file dirname [info script]] sdr_tx_iq_probe.xdc]
add_files -norecurse -fileset constrs_1 $_xdc
set_property used_in_synthesis false [get_files $_xdc]
puts "sdr_tx_iq_probe: added $_xdc (implementation-only)"
