# validate_bd.tcl -- self-contained build + validate of the SDR datalink fabric.
#
#   vivado -mode batch -source fpga/bd/validate_bd.tcl
#
# Why this exists separately from pluto_sdr_bd.tcl: the real design instantiates
# ADI's axi_ad9361, util_cpack2 and axi_dmac, none of which are in this
# repository. A block design naming them cannot be validated on a machine that
# only has Vivado and this project. So this harness builds the same topology
# with a datapath-only stub in place of the radio core, which leaves everything
# this project authors under test -- adapters, AXI-Lite fabric, SmartConnect and
# HP sharing, clock and reset topology, DMA attachment, address map.
#
# It validates the wiring. It does not validate the radio.

set root      [file normalize [file join [file dirname [info script]] ..]]
set proj_dir  [expr {[info exists ::env(SDR_BD_DIR)] ? $::env(SDR_BD_DIR) : "/tmp/sdr_bd_validate"}]
set part      [expr {[info exists ::env(SDR_HLS_PART)] ? $::env(SDR_HLS_PART) : "xc7z020clg400-2"}]

file delete -force $proj_dir
create_project sdr_bd_validate $proj_dir -part $part -force

# HLS IP. These paths follow the split into two projects; the old single
# qpsk_modem/solution_* layout has not existed since the cores were separated,
# and pointing at it is why the previous script found no IP at all.
set_property ip_repo_paths [list \
    [file join $root hls qpsk_modem qpsk_demod solution1 impl ip] \
    [file join $root hls qpsk_modem qpsk_mod   solution1 impl ip] \
] [current_project]
update_ip_catalog -rebuild

add_files -norecurse [glob [file join $root rtl *.v]]
add_files -norecurse [file join $root rtl stub axi_ad9361_stub.v]
update_compile_order -fileset sources_1

create_bd_design "sdr_datalink"

# ── Processing system ────────────────────────────────────────────────────
# HP0 and HP1 are separate ports, one per DMA direction. The previous script
# tied both DMA masters to HP0 with no interconnect between them, which is not
# a legal topology: two masters cannot share one slave port directly.
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP1 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
    CONFIG.PCW_S_AXI_HP1_DATA_WIDTH {64} \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100.0} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR {1}] $ps

# ── Radio stub, and the sample clock domain ──────────────────────────────
set radio [create_bd_cell -type module -reference axi_ad9361_stub axi_ad9361]

# The DSP runs in the converter's l_clk domain, because that is where samples
# arrive already qualified by adc_valid and no crossing is needed. That places a
# hard ceiling on it: qpsk_demod_top closes at 44.41 MHz, so l_clk must stay
# below that. At the 8 MS/s this link runs, it does, with 5x to spare -- but a
# Pluto reconfigured for 61.44 MS/s would violate it, and the failure would be
# silent corruption rather than a build error. Constrain, do not assume.
create_bd_port -dir I -type clk -freq_hz 8000000 l_clk_in
connect_bd_net [get_bd_ports l_clk_in] [get_bd_pins axi_ad9361/clk]

set rstgen [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rstgen_lclk]
connect_bd_net [get_bd_pins axi_ad9361/l_clk]  [get_bd_pins rstgen_lclk/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rstgen_lclk/ext_reset_in]

set rstgen_cpu [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rstgen_cpu]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0]     [get_bd_pins rstgen_cpu/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rstgen_cpu/ext_reset_in]

# ── Custom cores and adapters ────────────────────────────────────────────
set demod [create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_demod_top:2.0 qpsk_demod_0]
set modl  [create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_mod_top:2.0   qpsk_mod_0]
set iq2s  [create_bd_cell -type module -reference adi_iq_to_axis  iq_to_axis]
set s2iq  [create_bd_cell -type module -reference axis_to_adi_iq  axis_to_iq]
set pktz  [create_bd_cell -type module -reference axis_packetizer rx_packetizer]

# ── DMA: bytes only ──────────────────────────────────────────────────────
# S2MM carries demodulated BYTES and MM2S carries bytes to modulate. Raw IQ
# never leaves the fabric, which is the point of putting the modem here at all:
# at 8 MS/s complex 16-bit, streaming IQ to the ARM is 32 MB/s of DMA and a
# corresponding share of DDR bandwidth, to deliver 0.5 MB/s of payload.
set dma_rx [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma axi_dma_rx]
set_property -dict [list CONFIG.c_include_sg {0} CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} CONFIG.c_s2mm_burst_size {64} \
    CONFIG.c_m_axi_s2mm_data_width {64} CONFIG.c_s_axis_s2mm_tdata_width {8}] $dma_rx
set dma_tx [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma axi_dma_tx]
set_property -dict [list CONFIG.c_include_sg {0} CONFIG.c_include_mm2s {1} \
    CONFIG.c_include_s2mm {0} CONFIG.c_mm2s_burst_size {64} \
    CONFIG.c_m_axi_mm2s_data_width {64} CONFIG.c_m_axis_mm2s_tdata_width {8}] $dma_tx

# ── RX datapath: radio -> adapter -> demod -> packetizer -> DMA ──────────
foreach {src dst} {
    axi_ad9361/adc_valid_i0   iq_to_axis/adc_valid
    axi_ad9361/adc_enable_i0  iq_to_axis/adc_enable_i
    axi_ad9361/adc_enable_q0  iq_to_axis/adc_enable_q
    axi_ad9361/adc_data_i0    iq_to_axis/adc_data_i
    axi_ad9361/adc_data_q0    iq_to_axis/adc_data_q
} { connect_bd_net [get_bd_pins $src] [get_bd_pins $dst] }

connect_bd_intf_net [get_bd_intf_pins iq_to_axis/m_axis]     [get_bd_intf_pins qpsk_demod_0/s_axis_iq]
connect_bd_intf_net [get_bd_intf_pins qpsk_demod_0/m_axis_bits] [get_bd_intf_pins rx_packetizer/s_axis]
# The DSP runs on l_clk and the DMA on the CPU clock, so the byte streams
# cross a clock domain. That crossing is made explicit with stream clock
# converters rather than left to the DMA: AXI DMA's stream ports are
# synchronous to its own AXI clock unless the core is put in asynchronous
# mode, and assuming otherwise is what validate_bd_design caught here --
# "CLK_DOMAIN does not match between /axi_dma_rx/S_AXIS_S2MM and
# /rx_packetizer/m_axis". Putting the DMA in async mode would drag its
# memory-mapped side down to the 8 MHz sample clock as well; a converter
# leaves the DDR side running at full speed and costs one small FIFO.
set rx_cc [create_bd_cell -type ip -vlnv xilinx.com:ip:axis_clock_converter rx_cc]
set_property -dict [list CONFIG.TDATA_NUM_BYTES {1} CONFIG.HAS_TLAST {1} \
                        CONFIG.HAS_TKEEP {1}] $rx_cc
connect_bd_intf_net [get_bd_intf_pins rx_packetizer/m_axis] [get_bd_intf_pins rx_cc/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins rx_cc/M_AXIS]         [get_bd_intf_pins axi_dma_rx/S_AXIS_S2MM]

# ── TX datapath: DMA -> modulator -> adapter -> radio ────────────────────
# Order corrected. The previous script put the IQ gain block BEFORE the
# modulator, feeding an IQ-typed block with bytes and then feeding the
# modulator's byte input with IQ. Pulse shaping produces IQ; anything that
# scales IQ has to come after it.
set tx_cc [create_bd_cell -type ip -vlnv xilinx.com:ip:axis_clock_converter tx_cc]
set_property -dict [list CONFIG.TDATA_NUM_BYTES {1} CONFIG.HAS_TLAST {1} \
                        CONFIG.HAS_TKEEP {1}] $tx_cc
connect_bd_intf_net [get_bd_intf_pins axi_dma_tx/M_AXIS_MM2S] [get_bd_intf_pins tx_cc/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins tx_cc/M_AXIS]           [get_bd_intf_pins qpsk_mod_0/s_axis_bits]
connect_bd_intf_net [get_bd_intf_pins qpsk_mod_0/m_axis_iq]   [get_bd_intf_pins axis_to_iq/s_axis]
foreach {src dst} {
    axi_ad9361/dac_valid_i0   axis_to_iq/dac_valid
    axi_ad9361/dac_enable_i0  axis_to_iq/dac_enable_i
    axi_ad9361/dac_enable_q0  axis_to_iq/dac_enable_q
} { connect_bd_net [get_bd_pins $src] [get_bd_pins $dst] }
connect_bd_net [get_bd_pins axis_to_iq/dac_data_i] [get_bd_pins axi_ad9361/dac_data_i0]
connect_bd_net [get_bd_pins axis_to_iq/dac_data_q] [get_bd_pins axi_ad9361/dac_data_q0]

# ── AXI-Lite fabric ──────────────────────────────────────────────────────
# SmartConnect, not axi_interconnect, and sized for every slave. The previous
# script declared five master ports for six AXI-Lite slaves and left qpsk_mod
# off the list entirely -- so its `enabled` register would have read 0 forever
# and the transmitter would have emitted silence with nothing to indicate why.
set axil [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axil_sc]
# Two clocks: the HLS cores' AXI-Lite runs on their ap_clk, which is l_clk,
# while the PS master is on FCLK_CLK0. SmartConnect crosses that internally
# once it is told there are two domains; with NUM_CLKS 1 it reports the
# mismatch as a critical warning and validation fails.
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {4} CONFIG.NUM_CLKS {2}] $axil
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] [get_bd_intf_pins axil_sc/S00_AXI]
# The HLS AXI-Lite pin is s_axi_ctrl (named after the `bundle=ctrl` pragma),
# not `ctrl`. The old script used `ctrl` and would have failed on every lookup.
connect_bd_intf_net [get_bd_intf_pins axil_sc/M00_AXI] [get_bd_intf_pins qpsk_demod_0/s_axi_ctrl]
connect_bd_intf_net [get_bd_intf_pins axil_sc/M01_AXI] [get_bd_intf_pins qpsk_mod_0/s_axi_ctrl]
connect_bd_intf_net [get_bd_intf_pins axil_sc/M02_AXI] [get_bd_intf_pins axi_dma_rx/S_AXI_LITE]
connect_bd_intf_net [get_bd_intf_pins axil_sc/M03_AXI] [get_bd_intf_pins axi_dma_tx/S_AXI_LITE]

# ── DMA memory path: one HP port each, through SmartConnect ──────────────
set mem_sc [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect mem_sc]
set_property -dict [list CONFIG.NUM_SI {2} CONFIG.NUM_MI {2}] $mem_sc
connect_bd_intf_net [get_bd_intf_pins axi_dma_rx/M_AXI_S2MM] [get_bd_intf_pins mem_sc/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_dma_tx/M_AXI_MM2S] [get_bd_intf_pins mem_sc/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins mem_sc/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP0]
connect_bd_intf_net [get_bd_intf_pins mem_sc/M01_AXI] [get_bd_intf_pins ps7/S_AXI_HP1]

# ── Clocks and resets ────────────────────────────────────────────────────
# Absent entirely from the previous script, which is on its own enough to make
# validate_bd_design fail.
set lclk  [get_bd_pins axi_ad9361/l_clk]
set lrstn [get_bd_pins rstgen_lclk/peripheral_aresetn]
set cclk  [get_bd_pins ps7/FCLK_CLK0]
set crstn [get_bd_pins rstgen_cpu/peripheral_aresetn]

foreach p {iq_to_axis/clk axis_to_iq/clk rx_packetizer/clk qpsk_demod_0/ap_clk qpsk_mod_0/ap_clk} {
    connect_bd_net $lclk [get_bd_pins $p]
}
foreach p {iq_to_axis/resetn axis_to_iq/resetn rx_packetizer/resetn qpsk_demod_0/ap_rst_n qpsk_mod_0/ap_rst_n} {
    connect_bd_net $lrstn [get_bd_pins $p]
}
# The DMAs and the AXI fabric live in the CPU clock domain; the AXI-Stream
# ports of each DMA are crossed by the DMA's own async FIFOs.
foreach p {axi_dma_rx/s_axi_lite_aclk axi_dma_tx/s_axi_lite_aclk
           axi_dma_rx/m_axi_s2mm_aclk axi_dma_tx/m_axi_mm2s_aclk
           axil_sc/aclk mem_sc/aclk ps7/S_AXI_HP0_ACLK ps7/S_AXI_HP1_ACLK
           ps7/M_AXI_GP0_ACLK
           rx_cc/m_axis_aclk tx_cc/s_axis_aclk} {
    connect_bd_net $cclk [get_bd_pins $p]
}
foreach p {axi_dma_rx/axi_resetn axi_dma_tx/axi_resetn axil_sc/aresetn mem_sc/aresetn
           rx_cc/m_axis_aresetn tx_cc/s_axis_aresetn} {
    connect_bd_net $crstn [get_bd_pins $p]
}
# axil_sc/aclk1 is the second SmartConnect domain, feeding the HLS slaves.
connect_bd_net $lclk [get_bd_pins axil_sc/aclk1]
connect_bd_net $lclk [get_bd_pins rx_cc/s_axis_aclk]
connect_bd_net $lclk [get_bd_pins tx_cc/m_axis_aclk]
connect_bd_net $lrstn [get_bd_pins rx_cc/s_axis_aresetn]
connect_bd_net $lrstn [get_bd_pins tx_cc/m_axis_aresetn]

# ── Address map ──────────────────────────────────────────────────────────
assign_bd_address
foreach {seg off} {
    SEG_qpsk_demod_0_Reg 0x43C00000
    SEG_qpsk_mod_0_Reg   0x43C10000
    SEG_axi_dma_rx_Reg   0x43C20000
    SEG_axi_dma_tx_Reg   0x43C30000
} {
    set s [get_bd_addr_segs -quiet ps7/Data/$seg]
    if {[llength $s]} { set_property offset $off $s ; set_property range 64K $s }
}

validate_bd_design
save_bd_design
puts "SDR_BD_VALIDATE: OK"
