# sdr_dac_pin_probe.tcl -- TEMPORARY. Source AFTER sdr_insert.tcl.
#
# Taps the PARALLEL DAC pins, downstream of axis_to_adi_iq. Every other probe
# in this design sits on an AXI4-Stream upstream of that adapter, so none of
# them can see the one span the evidence implicates: the TX IQ probe correlates
# 1.000000 with mod_ref.iq while a second radio shows the air carrying QPSK
# whose symbols match the payload at no alignment, rotation or conjugation.
#
# Observational only -- the probe drives nothing, it just samples the nets that
# already run from axis_to_iq into axi_ad9361.
puts "sdr_dac_pin_probe: tapping dac_data_i0/q0 after axis_to_iq"

create_bd_cell -type module -reference dac_pin_probe dac_probe

# Attach to the EXISTING nets rather than re-wiring them: ad_connect on a pin
# that already has a net adds this cell as another consumer.
ad_connect axis_to_iq/dac_data_i   dac_probe/dac_data_i
ad_connect axis_to_iq/dac_data_q   dac_probe/dac_data_q
ad_connect axi_ad9361/dac_valid_i0 dac_probe/dac_valid

# l_clk domain: the same clock axis_to_adi_iq and the converter use.
ad_connect axi_ad9361/l_clk dac_probe/clk
ad_connect sdr_rst_inv/Res  dac_probe/resetn

ad_ip_instance axi_gpio dac_probe_gpio [list \
  C_IS_DUAL 1 C_ALL_OUTPUTS 1 C_ALL_INPUTS_2 1 \
  C_GPIO_WIDTH 32 C_GPIO2_WIDTH 32]
ad_connect dac_probe_gpio/gpio_io_o  dac_probe/ctrl
ad_connect dac_probe_gpio/gpio2_io_i dac_probe/stat
# 0x43C40000 is unmapped in this design (verified on hardware: it bus-errors
# while 0x43C0/10/20/30 all respond), so it is free.
ad_cpu_interconnect 0x43C40000 dac_probe_gpio

puts "sdr_dac_pin_probe: gpio at 0x43C40000 (ctrl +0x00, stat +0x08)"
