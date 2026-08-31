# sdr_tx_pattern.tcl -- TEMPORARY. Source AFTER sdr_insert.tcl and
# sdr_tx_iq_probe.tcl.
#
# Replaces the DMA byte feed into qpsk_mod with an in-fabric ROM of
# vectors/mod_ref.bits. The modulator then cannot starve, so the captured IQ is
# continuous signal rather than the startup gaps every DMA-fed attempt caught.
# This isolates "is the pulse-shaped IQ correct" from "does the byte-DMA
# transport work", which are separate questions that were being answered
# together and therefore not at all.
#
# Delete the source line to restore the DMA feed.
puts "sdr_tx_pattern: feeding qpsk_mod from an in-fabric mod_ref.bits ROM"

create_bd_cell -type module -reference tx_pattern_gen tx_pattern_gen

# Detach the DMA-side feed; the generator drives the modulator instead.
set n [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins qpsk_mod_0/s_axis_bits]]
if {[llength $n]} { delete_bd_objs $n }
ad_connect tx_pattern_gen/m_axis qpsk_mod_0/s_axis_bits

# Same clock as the modulator.
ad_connect $modem_clk  tx_pattern_gen/clk
ad_connect $modem_rstn tx_pattern_gen/resetn

# enable = bit 29 of the probe's existing control word, so no second AXI slave.
#   ctrl[31] arm   ctrl[30] status-select   ctrl[29] pattern enable
ad_ip_instance xlslice pat_en [list DIN_WIDTH 32 DIN_FROM 29 DIN_TO 29 DOUT_WIDTH 1]
ad_connect tx_iq_probe_gpio/gpio_io_o pat_en/Din
ad_connect pat_en/Dout tx_pattern_gen/enable

# tx_narrow's master is now unused. Leaving an AXI-Stream master dangling is
# only a warning, and keeping the DMA path built means restoring it is a
# one-line change rather than a rebuild of the transport.
puts "sdr_tx_pattern: qpsk_mod now fed from ROM; ctrl bit29 enables it"
