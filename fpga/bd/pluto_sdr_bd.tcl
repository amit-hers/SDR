# pluto_sdr_bd.tcl  —  Vivado IP Integrator block diagram for PlutoSDR SDR-Datalink
#
# Based on ADI HDL reference design for PlutoSDR (analogdevicesinc/hdl, branch main).
# Inserts four custom HLS IP cores into the existing data path:
#
#   RX path:
#     cf-ad9361-lpc  →  [rssi_meter]  →  [gain_block(RX)]  →  [qpsk_demod]
#                                     →  [sync_detector]   →  AXI DMA  →  PS DDR
#
#   TX path:
#     PS DDR  →  AXI DMA  →  [gain_block(TX)]  →  [qpsk_mod]  →  cf-ad9361-dds-core-lpc
#
#   AXI4-Lite (all IPs):
#     PS M_AXI_GP0 → AXI Interconnect → rssi_meter | gain_block_rx | gain_block_tx
#                                      | qpsk_demod | sync_detector
#
# Usage:
#   1. Clone ADI HDL: git clone https://github.com/analogdevicesinc/hdl
#   2. Open pluto project: cd hdl/projects/pluto && vivado -source system_project.tcl
#   3. In the TCL console: source <path>/fpga/bd/pluto_sdr_bd.tcl
#   4. Add each HLS IP to catalog first:
#        set_property ip_repo_paths {
#            fpga/hls/rssi_meter/rssi_meter/solution1/impl/ip
#            fpga/hls/gain_block/gain_block/solution1/impl/ip
#            fpga/hls/sync_detector/sync_detector/solution1/impl/ip
#            fpga/hls/qpsk_modem/qpsk_modem/solution_demod/impl/ip
#            fpga/hls/qpsk_modem/qpsk_modem/solution_mod/impl/ip
#        } [current_project]
#        update_ip_catalog
#
# AXI4-Lite base addresses (PS GP0 view):
#   rssi_meter        0x43C0_0000  (4 KB)
#   gain_block_rx     0x43C1_0000  (4 KB)
#   gain_block_tx     0x43C2_0000  (4 KB)
#   qpsk_demod        0x43C3_0000  (4 KB)
#   sync_detector     0x43C4_0000  (4 KB)
#
# To read from Linux:
#   devmem2 0x43C00018 w      # read rssi_meter power_accum
#   devmem2 0x43C40004 w      # read sync_detector match_count

proc sdr_create_bd {} {
    # ── Create top-level BD ───────────────────────────────────────────
    set bd_name "sdr_datalink_bd"
    create_bd_design $bd_name

    # ── Zynq PS ───────────────────────────────────────────────────────
    set zynq [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]
    apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
        -config {make_external "FIXED_IO, DDR" apply_board_preset "1"} $zynq
    set_property -dict [list \
        CONFIG.PCW_USE_M_AXI_GP0 {1} \
        CONFIG.PCW_USE_S_AXI_HP0 {1} \
        CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64}] $zynq

    # ── AXI DMA (RX: PS ← FPGA) ──────────────────────────────────────
    set dma_rx [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma_rx]
    set_property -dict [list \
        CONFIG.c_include_sg          {0} \
        CONFIG.c_sg_include_stscntrl_strm {0} \
        CONFIG.c_include_mm2s        {0} \
        CONFIG.c_include_s2mm        {1} \
        CONFIG.c_s2mm_burst_size     {256}] $dma_rx

    # ── AXI DMA (TX: PS → FPGA) ──────────────────────────────────────
    set dma_tx [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma_tx]
    set_property -dict [list \
        CONFIG.c_include_sg          {0} \
        CONFIG.c_sg_include_stscntrl_strm {0} \
        CONFIG.c_include_mm2s        {1} \
        CONFIG.c_include_s2mm        {0} \
        CONFIG.c_mm2s_burst_size     {256}] $dma_tx

    # ── ADI AXI AD9361 IP (from ADI HDL repo) ────────────────────────
    # NOTE: add axi_ad9361 from the ADI IP catalog before running this script
    set ad9361 [create_bd_cell -type ip -vlnv analog.com:user:axi_ad9361:1.0 axi_ad9361_0]
    set_property CONFIG.ID {0} $ad9361

    # ── HLS IP cores ─────────────────────────────────────────────────
    set rssi   [create_bd_cell -type ip -vlnv sdr-link:dsp:rssi_meter_top:1.0    rssi_meter_0]
    set gain_rx [create_bd_cell -type ip -vlnv sdr-link:dsp:gain_block_top:1.0   gain_block_rx]
    set gain_tx [create_bd_cell -type ip -vlnv sdr-link:dsp:gain_block_top:1.0   gain_block_tx]
    set demod  [create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_demod_top:2.0   qpsk_demod_0]
    set sync   [create_bd_cell -type ip -vlnv sdr-link:dsp:sync_detector_top:1.0 sync_det_0]
    set modtx  [create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_mod_top:2.0     qpsk_mod_0]

    # ── AXI4-Lite interconnect (GP0 → all HLS ctrl ports) ─────────────
    set axil_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axil_ic]
    set_property CONFIG.NUM_SI {1} $axil_ic
    set_property CONFIG.NUM_MI {5} $axil_ic

    # ── AXI4-Stream connections: RX path ─────────────────────────────
    # ad9361 RX → rssi_meter → gain_block_rx → qpsk_demod → sync_detector → dma_rx
    connect_bd_intf_net [get_bd_intf_pins axi_ad9361_0/adc_data]      \
                        [get_bd_intf_pins rssi_meter_0/s_axis_iq]
    connect_bd_intf_net [get_bd_intf_pins rssi_meter_0/m_axis_iq]     \
                        [get_bd_intf_pins gain_block_rx/s_axis_iq]
    connect_bd_intf_net [get_bd_intf_pins gain_block_rx/m_axis_iq]    \
                        [get_bd_intf_pins qpsk_demod_0/s_axis_iq]
    connect_bd_intf_net [get_bd_intf_pins qpsk_demod_0/m_axis_bits]   \
                        [get_bd_intf_pins sync_det_0/s_axis]
    connect_bd_intf_net [get_bd_intf_pins sync_det_0/m_axis]          \
                        [get_bd_intf_pins axi_dma_rx/S_AXIS_S2MM]

    # ── AXI4-Stream connections: TX path ─────────────────────────────
    # dma_tx → gain_block_tx → qpsk_mod → ad9361 TX
    connect_bd_intf_net [get_bd_intf_pins axi_dma_tx/M_AXIS_MM2S]     \
                        [get_bd_intf_pins gain_block_tx/s_axis_iq]
    connect_bd_intf_net [get_bd_intf_pins gain_block_tx/m_axis_iq]    \
                        [get_bd_intf_pins qpsk_mod_0/s_axis_bits]
    connect_bd_intf_net [get_bd_intf_pins qpsk_mod_0/m_axis_iq]       \
                        [get_bd_intf_pins axi_ad9361_0/dac_data]

    # ── AXI4-Lite connections (GP0 → HLS ctrl) ────────────────────────
    connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0]              \
                        [get_bd_intf_pins axil_ic/S00_AXI]
    set mi_list {rssi_meter_0 gain_block_rx gain_block_tx qpsk_demod_0 sync_det_0}
    set mi_idx 0
    foreach ip $mi_list {
        set mi_port [format "M%02d_AXI" $mi_idx]
        connect_bd_intf_net [get_bd_intf_pins axil_ic/${mi_port}]     \
                            [get_bd_intf_pins ${ip}/ctrl]
        incr mi_idx
    }

    # ── AXI HP0 for DMA ──────────────────────────────────────────────
    connect_bd_intf_net [get_bd_intf_pins axi_dma_rx/M_AXI_S2MM]     \
                        [get_bd_intf_pins ps7/S_AXI_HP0]
    connect_bd_intf_net [get_bd_intf_pins axi_dma_tx/M_AXI_MM2S]     \
                        [get_bd_intf_pins ps7/S_AXI_HP0]

    # ── Address map ───────────────────────────────────────────────────
    assign_bd_address [get_bd_addr_segs rssi_meter_0/ctrl/Reg]
    set_property offset 0x43C00000 [get_bd_addr_segs ps7/Data/SEG_rssi_meter_0_Reg]
    set_property range  4K         [get_bd_addr_segs ps7/Data/SEG_rssi_meter_0_Reg]

    assign_bd_address [get_bd_addr_segs gain_block_rx/ctrl/Reg]
    set_property offset 0x43C10000 [get_bd_addr_segs ps7/Data/SEG_gain_block_rx_Reg]
    set_property range  4K         [get_bd_addr_segs ps7/Data/SEG_gain_block_rx_Reg]

    assign_bd_address [get_bd_addr_segs gain_block_tx/ctrl/Reg]
    set_property offset 0x43C20000 [get_bd_addr_segs ps7/Data/SEG_gain_block_tx_Reg]
    set_property range  4K         [get_bd_addr_segs ps7/Data/SEG_gain_block_tx_Reg]

    assign_bd_address [get_bd_addr_segs qpsk_demod_0/ctrl/Reg]
    set_property offset 0x43C30000 [get_bd_addr_segs ps7/Data/SEG_qpsk_demod_0_Reg]
    set_property range  4K         [get_bd_addr_segs ps7/Data/SEG_qpsk_demod_0_Reg]

    assign_bd_address [get_bd_addr_segs sync_det_0/ctrl/Reg]
    set_property offset 0x43C40000 [get_bd_addr_segs ps7/Data/SEG_sync_det_0_Reg]
    set_property range  4K         [get_bd_addr_segs ps7/Data/SEG_sync_det_0_Reg]

    # ── Validate and save ─────────────────────────────────────────────
    validate_bd_design
    save_bd_design
    puts "Block diagram '${bd_name}' created and validated."
    puts ""
    puts "Next: Vivado → Generate Bitstream"
    puts "Then: copy boot.bin to PlutoSDR SD card or use pluto_update.sh"
}

sdr_create_bd
