# sdr_insert.tcl -- splice the QPSK modem into ADI's PlutoSDR block design.
#
# Sourced from the END of ADI's projects/pluto/system_bd.tcl, so ADI builds its
# reference design first and this reshapes the datapath afterwards. That way the
# radio bring-up (axi_ad9361, clocking, SPI, DDR, TDD) stays exactly as ADI
# ships and tests it, and only the sample transport changes.
#
# The modem cores themselves are untouched: they enter as packaged HLS IP built
# from fpga/hls/qpsk_modem, and nothing here re-synthesises or re-parameterises
# them.
#
# WHAT CHANGES: ADI moves raw IQ to userspace through util_cpack2/util_upack2
# and its two axi_dmac instances -- that transport IS libiio's cf-ad9361-lpc
# device. Here both DMAs are repurposed to carry BYTES, and IQ never leaves the
# fabric. At 8 MS/s complex 16-bit that removes 32 MB/s of DMA and DDR traffic
# that existed only to deliver 0.5 MB/s of payload. The cost is that the stock
# IIO buffer interface no longer returns samples: userspace reads demodulated
# bytes. This is a fork of the reference design, not an addition to it.

set sdr_repo [file normalize [file join [file dirname [info script]] .. ..]]
puts "sdr_insert: repo root $sdr_repo"

# ── HLS IP into the catalog ──────────────────────────────────────────────
# One project per core since the split; qpsk_modem/solution_* has not existed
# since, and pointing at it yields no IP rather than an error.
# The HLS IP reaches the catalog by being visible inside ADI's own library
# tree (fpga/hls/.../impl/ip is symlinked to library/sdr_qpsk_{demod,mod}),
# NOT by adding an ip_repo_path here.
#
# That distinction was three failed builds. ADI sets ip_repo_paths and runs
# update_ip_catalog in adi_project_create, and only afterwards does
# create_bd_design + source system_bd.tcl. Adding a repo from inside
# system_bd.tcl is too late: the property takes, and update_ip_catalog -rebuild
# reports nothing wrong, but the definition is still absent at create_bd_cell.
# Placing the IP where ADI already scans sidesteps the ordering entirely.
#
# Two earlier diagnoses looked right and were not. Setting the property on
# [current_project] instead of [current_fileset] is genuinely wrong, but fixing
# it changed nothing here; and an isolated reproduction of the same calls
# succeeded, because it ran outside the BD-construction context that is the
# actual constraint.
# Fail here with a message that names the cause, rather than 60 lines later at
# create_bd_cell where the error only says the definition is missing.
# -filter {VLNV == ...}, not a bare name: get_ipdefs matches object names and a
# raw colon-separated VLNV never matches, so the bare form fails a healthy
# catalog -- which it did, costing one build.
foreach vlnv {sdr-link:dsp:qpsk_demod_top:2.0 sdr-link:dsp:qpsk_mod_top:2.0} {
  if {![llength [get_ipdefs -quiet -filter "VLNV == $vlnv"]]} {
    error "sdr_insert: $vlnv missing from catalog. Expected it via the symlink\
           in [file join $ad_hdl_dir library] -- check fpga/hls/build.sh ran\
           under the SAME Vivado version as this project (IP packaged by a\
           newer tool is silently ignored)."
  }
}
puts "sdr_insert: HLS IP in catalog ([llength [get_ipdefs -quiet *sdr-link*]] defs)"

# ── Adapter RTL ──────────────────────────────────────────────────────────
add_files -norecurse -fileset sources_1 [glob [file join $sdr_repo fpga rtl *.v]]
update_compile_order -fileset sources_1

# ── Remove ADI's IQ transport ────────────────────────────────────────────
# cpack/upack and the sample-rate FIRs exist only to move IQ to and from the
# ARM. With the modem in fabric nothing consumes them, and leaving them
# instantiated but unconnected fails validation. Deleting the FIRs is also
# what frees the DSPs the demodulator needs.
foreach c {cpack tx_upack rx_fir_decimator tx_fir_interpolator \
           decim_slice interp_slice logic_or} {
  set obj [get_bd_cells -quiet $c]
  if {[llength $obj]} { delete_bd_objs $obj ; puts "sdr_insert: removed $c" }
}

# ── DMAs carry bytes now ─────────────────────────────────────────────────
# axi_dmac takes an AXI4-Stream directly, so no second DMA pair is needed.
#
# 16 bits per beat, not 8: axi_dmac rejects a byte-wide stream outright --
#   "Value '8' is out of the range for parameter Bus Width(DMA_DATA_WIDTH_SRC).
#    Valid values are - 16, 32, 64, ..."
# The modem cores are byte-wide and are not to be modified, so the width step
# happens outside them, in an axis_dwidth_converter on each side.
set_property -dict [list CONFIG.DMA_TYPE_SRC  {1} CONFIG.DMA_DATA_WIDTH_SRC  {16}] \
  [get_bd_cells axi_ad9361_adc_dma]
set_property -dict [list CONFIG.DMA_TYPE_DEST {1} CONFIG.DMA_DATA_WIDTH_DEST {16}] \
  [get_bd_cells axi_ad9361_dac_dma]

# Switching SRC from FIFO to stream replaces fifo_wr* with s_axis*, so ADI's
# earlier `ad_connect l_clk adc_dma/fifo_wr_clk` no longer has a port to land
# on. The stream clock is re-established below, in the modem clock domain.

# ── Modem and adapters ───────────────────────────────────────────────────
create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_demod_top:2.0 qpsk_demod_0
create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_mod_top:2.0   qpsk_mod_0
create_bd_cell -type module -reference adi_iq_to_axis  iq_to_axis
create_bd_cell -type module -reference axis_to_adi_iq  axis_to_iq
create_bd_cell -type module -reference axis_packetizer rx_packetizer

# ── RX: radio -> adapter -> demod -> packetizer -> DMA ───────────────────
ad_connect axi_ad9361/adc_valid_i0  iq_to_axis/adc_valid
ad_connect axi_ad9361/adc_enable_i0 iq_to_axis/adc_enable_i
ad_connect axi_ad9361/adc_enable_q0 iq_to_axis/adc_enable_q
ad_connect axi_ad9361/adc_data_i0   iq_to_axis/adc_data_i
ad_connect axi_ad9361/adc_data_q0   iq_to_axis/adc_data_q
ad_connect qpsk_demod_0/m_axis_bits rx_packetizer/s_axis

# ── TX: DMA -> modulator -> adapter -> radio ─────────────────────────────
# Modulator first, then anything touching IQ: pulse shaping PRODUCES IQ.

ad_connect axi_ad9361/dac_valid_i0   axis_to_iq/dac_valid
ad_connect axi_ad9361/dac_enable_i0  axis_to_iq/dac_enable_i
ad_connect axi_ad9361/dac_enable_q0  axis_to_iq/dac_enable_q
ad_connect axis_to_iq/dac_data_i     axi_ad9361/dac_data_i0
ad_connect axis_to_iq/dac_data_q     axi_ad9361/dac_data_q0

# The second AD9361 channel was fed by upack; with that gone its data inputs
# would float. Tie them low rather than leave them undriven.
ad_ip_instance xlconstant dac_ch1_zero [list CONST_VAL 0 CONST_WIDTH 16]
ad_connect dac_ch1_zero/dout axi_ad9361/dac_data_i1
ad_connect dac_ch1_zero/dout axi_ad9361/dac_data_q1

# Overflow/underflow keep their meaning: the adapters detect exactly what
# cpack/upack used to, so they drive the same AD9361 status inputs.
ad_connect iq_to_axis/overflow  axi_ad9361/adc_dovf
ad_connect axis_to_iq/underflow axi_ad9361/dac_dunf

# ── Two clock domains, on purpose ────────────────────────────────────────
#
# l_clk is NOT the sample rate. It is the AD9361 data-interface clock, and ADI
# constrains it for the converter's maximum rate -- 61.463 MHz on this design
# (rx_clk, 16.270 ns) -- irrespective of the baseband rate actually in use.
# Running the modem there required it to close at 61.46 MHz. It does not: the
# routed Costas recurrence (phase -> index multiply -> NCO ROM) measured
# 26.881 ns, i.e. 37.2 MHz, and implementation reported
#   Slack (VIOLATED): -11.229ns ... Path Group: rx_clk
#
# The modem's throughput requirement is set by the SAMPLE rate (~8 MS/s), not
# by the interface clock, so it belongs in its own domain. At 40 MHz it has
# roughly 5x the headroom it needs, and the carrier loop stays exactly as
# validated -- no gain changes, no NCO rework.
#
# Only the two IQ sample streams cross domains, through async clock
# converters. Everything byte-side runs at the modem clock, so the DMA path
# needs no second crossing.
ad_ip_parameter sys_ps7 CONFIG.PCW_EN_CLK2_PORT 1
ad_ip_parameter sys_ps7 CONFIG.PCW_FPGA2_PERIPHERAL_FREQMHZ 40.0
ad_ip_instance proc_sys_reset modem_rstgen
ad_connect sys_ps7/FCLK_CLK2     modem_rstgen/slowest_sync_clk
ad_connect sys_ps7/FCLK_RESET0_N modem_rstgen/ext_reset_in
set modem_clk  sys_ps7/FCLK_CLK2
set modem_rstn modem_rstgen/peripheral_aresetn

# The adapters stay on l_clk: they touch the AD9361's parallel ports, which
# only exist in that domain. ADI supplies an active-high rst; the adapters and
# HLS cores want active-low.
ad_ip_instance util_vector_logic sdr_rst_inv [list C_OPERATION not C_SIZE 1]
ad_connect axi_ad9361/rst sdr_rst_inv/Op1
foreach p {iq_to_axis axis_to_iq} {
  ad_connect axi_ad9361/l_clk $p/clk
  ad_connect sdr_rst_inv/Res  $p/resetn
}

# Modem, packetiser, width converters and both DMA stream ports: modem clock.
foreach p {qpsk_demod_0 qpsk_mod_0} {
  ad_connect $modem_clk  $p/ap_clk
  ad_connect $modem_rstn $p/ap_rst_n
}
ad_connect $modem_clk  rx_packetizer/clk
ad_connect $modem_rstn rx_packetizer/resetn

# Both DMA stream clocks were already driven from l_clk by ADI's script, so
# they must be released before being re-driven -- connect_bd_net refuses a
# second source outright:
#   ERROR: [BD 5-676] The sink </axi_ad9361_dac_dma/m_axis_aclk> is already
#   connected to another source
proc sdr_rewire {pin src} {
  set bp [get_bd_pins -quiet $pin]
  if {![llength $bp]} { return }
  set n [get_bd_nets -quiet -of_objects $bp]
  if {[llength $n]} { disconnect_bd_net $n $bp }
  ad_connect $src $bp
}
sdr_rewire axi_ad9361_adc_dma/s_axis_aclk $modem_clk
sdr_rewire axi_ad9361_dac_dma/m_axis_aclk $modem_clk

# ── IQ sample streams cross l_clk <-> modem clock ────────────────────────
# RX: the ADC has no backpressure, so the converter's FIFO plus iq_to_axis's
# overflow flag are what stand between a stalled sink and silently dropped
# samples. At 8 MS/s into a 40 MHz sink the FIFO drains ~5x faster than it
# fills, so it should never fill -- and if it ever does, adc_dovf reports it.
ad_ip_instance axis_clock_converter rx_iq_cc \
  [list TDATA_NUM_BYTES 4 HAS_TLAST 1 HAS_TKEEP 1]
ad_connect axi_ad9361/l_clk rx_iq_cc/s_axis_aclk
ad_connect sdr_rst_inv/Res  rx_iq_cc/s_axis_aresetn
ad_connect $modem_clk       rx_iq_cc/m_axis_aclk
ad_connect $modem_rstn      rx_iq_cc/m_axis_aresetn
ad_connect iq_to_axis/m_axis rx_iq_cc/S_AXIS
ad_connect rx_iq_cc/M_AXIS   qpsk_demod_0/s_axis_iq

# TX: dac_valid is a REQUEST, so this FIFO must never be empty when the DAC
# asks. The modulator produces one sample per modem clock (40 MHz) against a
# request rate of ~8 MS/s, so it outruns the DAC by ~5x and the FIFO stays
# fed after the initial fill. axis_to_iq/underflow latches the startup gap
# and any later starvation rather than letting it pass as silence.
ad_ip_instance axis_clock_converter tx_iq_cc \
  [list TDATA_NUM_BYTES 4 HAS_TLAST 1 HAS_TKEEP 1]
ad_connect $modem_clk       tx_iq_cc/s_axis_aclk
ad_connect $modem_rstn      tx_iq_cc/s_axis_aresetn
ad_connect axi_ad9361/l_clk tx_iq_cc/m_axis_aclk
ad_connect sdr_rst_inv/Res  tx_iq_cc/m_axis_aresetn
ad_connect qpsk_mod_0/m_axis_iq tx_iq_cc/S_AXIS
ad_connect tx_iq_cc/M_AXIS      axis_to_iq/s_axis

# ── Byte stream <-> DMA width conversion ─────────────────────────────────
# Placed after the reset inverter exists, since both converters need it.
ad_ip_instance axis_dwidth_converter rx_widen \
  [list M_TDATA_NUM_BYTES 2 S_TDATA_NUM_BYTES 1 HAS_TLAST 1 HAS_TKEEP 1]
ad_connect $modem_clk           rx_widen/aclk
ad_connect $modem_rstn          rx_widen/aresetn
ad_connect rx_packetizer/m_axis rx_widen/S_AXIS
ad_connect rx_widen/M_AXIS      axi_ad9361_adc_dma/s_axis

ad_ip_instance axis_dwidth_converter tx_narrow \
  [list M_TDATA_NUM_BYTES 1 S_TDATA_NUM_BYTES 2 HAS_TLAST 1 HAS_TKEEP 1]
ad_connect $modem_clk                tx_narrow/aclk
ad_connect $modem_rstn               tx_narrow/aresetn
ad_connect axi_ad9361_dac_dma/m_axis tx_narrow/S_AXIS
ad_connect tx_narrow/M_AXIS          qpsk_mod_0/s_axis_bits

# ── AXI-Lite control ─────────────────────────────────────────────────────
# ad_cpu_interconnect crosses to the l_clk domain itself. The HLS slave pin is
# s_axi_ctrl, named for the `bundle=ctrl` pragma.
#   qpsk_demod 0x43C0_0000 : 0x10 enable, 0x18 lock_count RO, 0x20 soft_reset
#   qpsk_mod   0x43C1_0000 : 0x10 enable, 0x14 bpsk_mode
# AXI-Lite crosses into l_clk inside ADI's interconnect, per master port.
#
# HLS gives a core one ap_clk for both datapath and AXI-Lite, and here ap_clk
# must be l_clk: that is where samples arrive, and the demodulator's 44.41 MHz
# closure rules out running its datapath at FCLK_CLK0's 100 MHz. ADI's helper
# assumes otherwise -- it does `ad_connect sys_cpu_clk $p_intf_clock` on the
# slave's associated clock -- which gave
#   ERROR: [BD 41-237] CLK_DOMAIN does not match between
#          /qpsk_demod_0/s_axi_ctrl(l_clk) and .../M06_AXI(FCLK_CLK0)
# ADI's own l_clk IP escapes this because axi_ad9361 has separate clocks for
# its register interface and its datapath; an HLS core has only the one.
#
# An axi_clock_converter in front of each core was tried and is the wrong tool
# here: it is a transparent pass-through with no address segment of its own, so
# ad_cpu_interconnect's segment discovery walked off into an unrelated segment
# and emitted
#   create_bd_addr_seg ... /sys_ps7/Data/SEG_data_axi_iic_main SEG_data_..._axi_cc
#   ERROR: [BD 41-80] Specified object '.../SEG_data_axi_iic_main' does not exist
#
# axi_gp0_interconnect is an axi_interconnect, and those carry an independent
# ACLK per master port, inserting clock conversion themselves when a port's
# clock differs. So attach the core normally -- addressing then resolves
# against the core's own segment, which is what ADI's code expects -- and
# afterwards move just that port onto the modem clock.
proc sdr_port_to_modem_clk {core clk rstn} {
  set slave [get_bd_intf_pins ${core}/s_axi_ctrl]
  set net   [get_bd_intf_nets -of_objects $slave]
  foreach e [get_bd_intf_pins -of_objects $net] {
    if {"$e" eq "$slave"} { continue }
    set cell [get_bd_cells -of_objects $e]
    set idx  [string range [lindex [split $e /] end] 0 2]
    foreach {pin src} [list ${idx}_ACLK $clk ${idx}_ARESETN $rstn] {
      set bp [get_bd_pins -quiet ${cell}/${pin}]
      if {![llength $bp]} { continue }
      set n [get_bd_nets -quiet -of_objects $bp]
      if {[llength $n]} { disconnect_bd_net $n $bp }
      ad_connect $src $bp
    }
    puts "sdr_insert: ${cell}/${idx} moved to modem clock for $core"
  }
}

ad_cpu_interconnect 0x43C00000 qpsk_demod_0
ad_cpu_interconnect 0x43C10000 qpsk_mod_0
sdr_port_to_modem_clk qpsk_demod_0 $modem_clk $modem_rstn
sdr_port_to_modem_clk qpsk_mod_0   $modem_clk $modem_rstn

puts "sdr_insert: modem spliced in (demod 0x43C00000, mod 0x43C10000)"
puts "sdr_insert: NOTE the ADI DMAs now carry bytes -- libiio will not return IQ"
