# ─────────────────────────────────────────────────────────────────────────
# AUTHORITATIVE AD9363 parallel-port pin map -- LibreSDR Rev.5 / Pluto+ (XC7Z020)
#
# FROZEN 2026-08-31. The RX map below is MEASURED, not inferred. Every earlier
# map on this board came from reading schematics or device-tree properties and
# every one of them was wrong, so this was settled empirically instead:
#
#   1. fpga/probe/probe.tcl    -- read all 32 candidate balls as pulled-down
#      inputs. Exactly 14 toggled: N20, U18 and the 12 data balls below. Only
#      P-halves are driven => the port is single-ended CMOS, physically. All
#      16 tx_* balls were quiet.
#   2. fpga/probe/pn_probe.tcl -- captured RX_FRAME + the 12 lanes synchronously
#      on DATA_CLK into BRAM while the AD9363 ran BIST (verified at the chip:
#      REG_BIST_CONFIG 0x3F4 read back 0x09).
#   3. fpga/probe/decode_pn.py -- the PN generator is a shift register whose 12
#      parallel taps feed the bus, so each lane is the next lane delayed by one
#      sample. Those 2-term XOR relations chain all 12 lanes into a single
#      order, which IS the bit order: 45 relations predicted, 0 contradictions.
#   4. Cross-checked on an independent BIST tone capture: this order
#      reconstructs a 99.93%-pure tone at the expected bin, full scale +/-2048,
#      0.52 LSB RMS from an ideal cosine. The reversed order gives 15%.
#
# NOTE ON THE PORT SWAP: an earlier map applied adi,swap-ports-enable by
# exchanging the RX and TX ball sets. That is wrong. The device-tree property
# IS present, but it configures the chip internally and does NOT imply the
# FPGA should exchange ball sets -- measurement shows the chip drives RX on
# the P1/RX_D* balls regardless. The pair convention (bit 2i = N half, 2i+1 = P half) was
# always correct; only the swap was not.
#
# The TX map is the complementary 16 balls, using the same pair convention. It
# is INFERRED, not measured -- the probe can only prove those balls are the
# ones the chip does not drive. Validate it with a TX test (dac_dunf) before
# trusting it.
#
# Bank VCCO is 2.5 V => LVCMOS25 (not the LVCMOS18 inherited from the 7z010
# plutoplus constraints).
# ─────────────────────────────────────────────────────────────────────────

# ---- RX: MEASURED ----
set_property -dict {PACKAGE_PIN N20 IOSTANDARD LVCMOS25} [get_ports rx_clk_in]
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS25} [get_ports rx_frame_in]
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[0]}]
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[1]}]
set_property -dict {PACKAGE_PIN V18 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[2]}]
set_property -dict {PACKAGE_PIN V17 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[3]}]
set_property -dict {PACKAGE_PIN W20 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[4]}]
set_property -dict {PACKAGE_PIN V20 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[5]}]
set_property -dict {PACKAGE_PIN R17 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[6]}]
set_property -dict {PACKAGE_PIN R16 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[7]}]
set_property -dict {PACKAGE_PIN W19 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[8]}]
set_property -dict {PACKAGE_PIN W18 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[9]}]
set_property -dict {PACKAGE_PIN W16 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[10]}]
set_property -dict {PACKAGE_PIN V16 IOSTANDARD LVCMOS25} [get_ports {rx_data_in[11]}]

# ---- TX: INFERRED (complementary balls, same pair convention) ----
set_property -dict {PACKAGE_PIN N18 IOSTANDARD LVCMOS25} [get_ports tx_clk_out]
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS25} [get_ports tx_frame_out]
set_property -dict {PACKAGE_PIN Y14 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[0]}]
set_property -dict {PACKAGE_PIN W14 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[1]}]
set_property -dict {PACKAGE_PIN U12 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[2]}]
set_property -dict {PACKAGE_PIN T12 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[3]}]
set_property -dict {PACKAGE_PIN U15 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[4]}]
set_property -dict {PACKAGE_PIN U14 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[5]}]
set_property -dict {PACKAGE_PIN U17 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[6]}]
set_property -dict {PACKAGE_PIN T16 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[7]}]
set_property -dict {PACKAGE_PIN W13 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[8]}]
set_property -dict {PACKAGE_PIN V12 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[9]}]
set_property -dict {PACKAGE_PIN W15 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[10]}]
set_property -dict {PACKAGE_PIN V15 IOSTANDARD LVCMOS25} [get_ports {tx_data_out[11]}]

# ---- control / SPI (verified: SPI made the AD9363A reachable, reg 0x37 = 0x0A)
set_property -dict {PACKAGE_PIN R18 IOSTANDARD LVCMOS25} [get_ports enable]
set_property -dict {PACKAGE_PIN P14 IOSTANDARD LVCMOS25} [get_ports txnrx]
set_property -dict {PACKAGE_PIN N17 IOSTANDARD LVCMOS25} [get_ports gpio_resetb]
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS25} [get_ports gpio_en_agc]
set_property -dict {PACKAGE_PIN P18 IOSTANDARD LVCMOS25 PULLTYPE PULLUP} [get_ports spi_csn]
set_property -dict {PACKAGE_PIN R14 IOSTANDARD LVCMOS25} [get_ports spi_clk]
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS25} [get_ports spi_mosi]
set_property -dict {PACKAGE_PIN R19 IOSTANDARD LVCMOS25} [get_ports spi_miso]

# 61.44 MHz DATA_CLK in CMOS mode (measured: fs = 30.72 MSPS, DATA_CLK = 2*fs;
# the BIST tone landed at exactly fs/32, confirming the rate).
create_clock -period 16.276 -name rx_clk [get_ports rx_clk_in]
