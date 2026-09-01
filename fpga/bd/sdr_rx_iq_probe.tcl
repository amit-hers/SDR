# sdr_rx_iq_probe.tcl -- TEMPORARY. Source AFTER sdr_insert.tcl.
#
# Mirrors the TX probe onto the RX path: captures the IQ that actually reaches
# qpsk_demod's input. The demodulator passes csim bit-exact yet reaches only
# ~48% symbol accuracy on any hardware-sourced signal -- identical in digital
# loopback (zero CFO, zero timing drift) and over the air, which rules out the
# channel and points at what the demod is being fed. The host demodulator
# recovers the same signal at BER 0, so the waveform on the wire is provably
# good; this instruments the last unobserved point.
#
# Combinational pass-through, like the TX probe, so inserting it cannot change
# the handshake or perturb what it measures.
puts "sdr_rx_iq_probe: splicing RX IQ capture at qpsk_demod input"

create_bd_cell -type module -reference iq_probe rx_iq_probe

set n [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins qpsk_demod_0/s_axis_iq]]
if {[llength $n]} { delete_bd_objs $n }
ad_connect rx_iq_cc/M_AXIS      rx_iq_probe/s_axis
ad_connect rx_iq_probe/m_axis   qpsk_demod_0/s_axis_iq

# rx_iq_cc's master side is already in the modem clock domain.
ad_connect $modem_clk  rx_iq_probe/clk
ad_connect $modem_rstn rx_iq_probe/resetn

ad_ip_instance axi_gpio rx_iq_probe_gpio [list \
  C_IS_DUAL 1 C_ALL_OUTPUTS 1 C_ALL_INPUTS_2 1 \
  C_GPIO_WIDTH 32 C_GPIO2_WIDTH 32]
ad_connect rx_iq_probe_gpio/gpio_io_o  rx_iq_probe/ctrl
ad_connect rx_iq_probe_gpio/gpio2_io_i rx_iq_probe/stat
ad_cpu_interconnect 0x43C30000 rx_iq_probe_gpio

puts "sdr_rx_iq_probe: gpio at 0x43C30000"
