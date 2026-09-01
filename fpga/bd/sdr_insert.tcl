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
# The modem clock is generated INSIDE the PL, not requested from the PS.
#
# Asking the PS for a new FCLK does not survive a PL-only reload. PS clock
# configuration lives in ps7_init, executed by the FSBL out of BOOT.bin at
# boot; a bitstream cannot change it. Setting PCW_FPGA2_PERIPHERAL_FREQMHZ in
# this block design only affects generated boot code, so on a board booted with
# the stock image FCLK_CLK2 kept the divisors the original design left it with:
#
#   FPGA2_CLK_CTRL 0xF8000190 = 0x00101800  -> div0=24, div1=1 -> 41.67 MHz
#
# against the 30.3 MHz this design is built for. The modem was overclocked by
# 33%, which is why its AXI-Lite answered while idle and then bus-errored once
# the core was started. Measured on hardware, not inferred.
#
# A Clocking Wizard off sys_cpu_clk removes the dependency entirely: the clock
# exists because the fabric makes it, whatever firmware the board booted. That
# also keeps the design loadable by the reversible PL-only path, which matters
# for bring-up -- the alternative is a DFU flash for every iteration.
# 30 MHz.
#
# An earlier version of this comment claimed the link regressed at 40 MHz and at
# II=1. That claim is WITHDRAWN. Those numbers were single runs, and the
# demodulator's lock turned out to be intermittent: two consecutive runs on a
# freshly booted board, same bitstream and same script, measured BER 8.8e-3 and
# then no sync at all. The "regressions" sit inside that spread, so they measured
# nothing about the clock or the schedule.
#
# Nothing in qpsk_demod observes its clock anyway: the AGC adapts per SAMPLE,
# the Costas and timing loops update per SYMBOL, and no counter ticks on clocks.
#
# 30 MHz is simply the rate this design has been exercised at. Raising it for
# more throughput is untested rather than known-bad -- but fix the acquisition
# problem first, or the measurement will not be trustworthy either way.
ad_ip_instance clk_wiz modem_clk_wiz [list \
  PRIM_IN_FREQ               100.000 \
  CLKOUT1_REQUESTED_OUT_FREQ  30.000 \
  USE_LOCKED                 true \
  USE_RESET                  false \
  PRIMITIVE                  MMCM]
ad_connect sys_cpu_clk modem_clk_wiz/clk_in1

# The reset generator is released by the MMCM's locked output, so nothing in
# the modem domain runs before its clock is stable.
ad_ip_instance proc_sys_reset modem_rstgen
ad_connect modem_clk_wiz/clk_out1 modem_rstgen/slowest_sync_clk
ad_connect modem_clk_wiz/locked   modem_rstgen/dcm_locked
ad_connect sys_ps7/FCLK_RESET0_N  modem_rstgen/ext_reset_in
set modem_clk  modem_clk_wiz/clk_out1
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
# ── Byte-stream elasticity between the DMA and the modulator ─────────────
# axi_dmac delivers in 4096-byte bursts with gaps between transfers. The
# modulator consumes one byte per 16 IQ samples (480 kB/s at 7.68 MS/s) and has
# no input buffer of its own, so a burst gap drains it and it emits zero-driven
# output -- `if (s_axis_bits.empty()) goto rrc_out`. Measured on hardware: the
# DMA sustained 475-483 kB/s and the captured waveform matched the frozen
# reference at correlation 1.000000 in two runs of three, but the third showed
# one 23-sample (~1.4 byte) starvation. The stream RESYNCHRONISED after it, so
# no bytes were lost -- it is a timing gap, not a data defect.
#
# This FIFO absorbs that gap. It sits in the byte transport, upstream of
# qpsk_mod, so the verified modem and IQ clock-converter path is untouched.
# 2048 bytes is ~4 ms of runway at 480 kB/s, far more than any observed gap.
ad_ip_instance axis_data_fifo tx_byte_fifo [list \
  TDATA_NUM_BYTES 1 FIFO_DEPTH 2048 HAS_TLAST 1 HAS_TKEEP 1 IS_ACLK_ASYNC 0]
ad_connect $modem_clk  tx_byte_fifo/s_axis_aclk
ad_connect $modem_rstn tx_byte_fifo/s_axis_aresetn
ad_connect tx_narrow/M_AXIS          tx_byte_fifo/S_AXIS
ad_connect tx_byte_fifo/M_AXIS       qpsk_mod_0/s_axis_bits

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
