# pluto_sdr_bd.tcl -- insert the QPSK modem into ADI's PlutoSDR reference design.
#
#   git clone https://github.com/analogdevicesinc/hdl
#   cd hdl/projects/pluto && vivado -mode batch -source system_project.tcl
#   # then, with the block design open:
#   source <this-repo>/fpga/bd/pluto_sdr_bd.tcl ; sdr_insert_datalink
#
# This PATCHES ADI's design rather than recreating it. The previous version of
# this file tried to build the radio path from scratch and got the interface
# wrong in every particular: axi_ad9361 does not expose AXI4-Stream `adc_data`
# / `dac_data` interfaces. It presents parallel adc_data_i0/q0 and dac_data_i0
# /q0, 16 bits each, qualified by adc_valid/adc_enable and dac_valid/dac_enable,
# in the l_clk domain, and ADI's own design packs them with util_cpack2 into
# axi_dmac. Everything downstream of that mistake -- the stream connections,
# the DMA choice, the clocking -- was wrong too.
#
# Verified against analogdevicesinc/hdl main:
#   projects/pluto/system_bd.tcl, library/axi_ad9361/axi_ad9361.v
#
# WHAT THIS CHANGES ABOUT THE SYSTEM, deliberately and visibly:
#
#   ADI's DMAs carry raw IQ to userspace, which is what libiio's cf-ad9361-lpc
#   device is. This repurposes both to carry BYTES instead, because the whole
#   point of putting the modem in fabric is that the ARM should never see IQ:
#   at 8 MS/s complex 16-bit that is 32 MB/s of DMA and DDR traffic to deliver
#   0.5 MB/s of payload. The consequence is that the stock IIO buffer interface
#   no longer yields samples -- userspace reads demodulated bytes. Anything
#   expecting to capture IQ through libiio needs the raw path restored, so
#   treat this as a fork of the reference design, not a drop-in addition.
#
# CLOCKING: the modem runs in the converter's l_clk domain, where samples
# already arrive qualified by adc_valid and no crossing is needed. qpsk_demod
# closes timing at 44.41 MHz, so l_clk MUST stay below that. At this link's
# 8 MS/s it does, with room to spare; a Pluto reconfigured for 61.44 MS/s
# would violate it. Check, do not assume.
#
# Register map added by this script (PS GP0):
#   qpsk_demod   0x43C0_0000   0x10 enable, 0x18 lock_count (RO), 0x20 soft_reset
#   qpsk_mod     0x43C1_0000   0x10 enable, 0x14 bpsk_mode
# ADI's own map is untouched: axi_ad9361 0x7902_0000, DMAs 0x7C40/0x7C42_0000.

proc sdr_insert_datalink {{repo_root ""}} {
    if {$repo_root eq ""} {
        set repo_root [file normalize [file join [file dirname [info script]] ..]]
    }

    # HLS IP. One project per core since the split -- the old path
    # qpsk_modem/solution_demod has not existed since, and pointing the catalog
    # at it silently yields no IP rather than an error.
    set_property ip_repo_paths [concat [get_property ip_repo_paths [current_project]] [list \
        [file join $repo_root hls qpsk_modem qpsk_demod solution1 impl ip] \
        [file join $repo_root hls qpsk_modem qpsk_mod   solution1 impl ip]]] [current_project]
    update_ip_catalog -rebuild

    add_files -norecurse [glob [file join $repo_root rtl *.v]]
    update_compile_order -fileset sources_1

    # ── Remove ADI's IQ transport ────────────────────────────────────────
    # cpack/upack and the sample-rate FIRs exist to move IQ to and from the
    # ARM. Nothing downstream of the modem wants them, and leaving them
    # instantiated but unconnected fails validation.
    foreach c {cpack tx_upack rx_fir_decimator tx_fir_interpolator decim_slice interp_slice logic_or} {
        set obj [get_bd_cells -quiet $c]
        if {[llength $obj]} { delete_bd_objs $obj }
    }

    # ── Repurpose ADI's DMAs to byte streams ─────────────────────────────
    # axi_dmac takes a stream directly, so no second pair of DMAs is needed --
    # which matters on a 7z020 already at 47% DSP with the modem in place.
    set_property -dict [list CONFIG.DMA_TYPE_SRC  {1} CONFIG.DMA_DATA_WIDTH_SRC  {8}] \
        [get_bd_cells axi_ad9361_adc_dma]
    set_property -dict [list CONFIG.DMA_TYPE_DEST {1} CONFIG.DMA_DATA_WIDTH_DEST {8}] \
        [get_bd_cells axi_ad9361_dac_dma]

    # ── Modem and adapters ───────────────────────────────────────────────
    create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_demod_top:2.0 qpsk_demod_0
    create_bd_cell -type ip -vlnv sdr-link:dsp:qpsk_mod_top:2.0   qpsk_mod_0
    create_bd_cell -type module -reference adi_iq_to_axis  iq_to_axis
    create_bd_cell -type module -reference axis_to_adi_iq  axis_to_iq
    create_bd_cell -type module -reference axis_packetizer rx_packetizer

    # ── RX: radio -> adapter -> demod -> packetizer -> DMA ───────────────
    foreach {src dst} {
        axi_ad9361/adc_valid_i0  iq_to_axis/adc_valid
        axi_ad9361/adc_enable_i0 iq_to_axis/adc_enable_i
        axi_ad9361/adc_enable_q0 iq_to_axis/adc_enable_q
        axi_ad9361/adc_data_i0   iq_to_axis/adc_data_i
        axi_ad9361/adc_data_q0   iq_to_axis/adc_data_q
    } { ad_connect $src $dst }
    ad_connect iq_to_axis/m_axis        qpsk_demod_0/s_axis_iq
    ad_connect qpsk_demod_0/m_axis_bits rx_packetizer/s_axis
    ad_connect rx_packetizer/m_axis     axi_ad9361_adc_dma/s_axis

    # ── TX: DMA -> modulator -> adapter -> radio ─────────────────────────
    # Modulator first, then anything that touches IQ. The old script had an IQ
    # gain block ahead of the modulator, feeding it IQ where it expects bytes.
    ad_connect axi_ad9361_dac_dma/m_axis qpsk_mod_0/s_axis_bits
    ad_connect qpsk_mod_0/m_axis_iq      axis_to_iq/s_axis
    foreach {src dst} {
        axi_ad9361/dac_valid_i0  axis_to_iq/dac_valid
        axi_ad9361/dac_enable_i0 axis_to_iq/dac_enable_i
        axi_ad9361/dac_enable_q0 axis_to_iq/dac_enable_q
    } { ad_connect $src $dst }
    ad_connect axis_to_iq/dac_data_i axi_ad9361/dac_data_i0
    ad_connect axis_to_iq/dac_data_q axi_ad9361/dac_data_q0

    # ── Clocks and resets: everything here is l_clk ──────────────────────
    # ADI's design already provides l_clk and an active-high rst; the adapters
    # and HLS cores want active-low, hence the inverter.
    create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic sdr_rst_inv
    set_property -dict [list CONFIG.C_OPERATION {not} CONFIG.C_SIZE {1}] [get_bd_cells sdr_rst_inv]
    ad_connect axi_ad9361/rst sdr_rst_inv/Op1
    foreach p {iq_to_axis axis_to_iq rx_packetizer} {
        ad_connect axi_ad9361/l_clk   $p/clk
        ad_connect sdr_rst_inv/Res    $p/resetn
    }
    foreach p {qpsk_demod_0 qpsk_mod_0} {
        ad_connect axi_ad9361/l_clk   $p/ap_clk
        ad_connect sdr_rst_inv/Res    $p/ap_rst_n
    }

    # ── AXI-Lite ─────────────────────────────────────────────────────────
    # ad_cpu_interconnect handles the clock-domain crossing to l_clk itself.
    # The HLS slave pin is s_axi_ctrl -- named for the `bundle=ctrl` pragma --
    # not `ctrl`, which is what the old script asked for on every core.
    ad_cpu_interconnect 0x43C00000 qpsk_demod_0
    ad_cpu_interconnect 0x43C10000 qpsk_mod_0

    validate_bd_design
    save_bd_design
    puts "qpsk modem inserted: demod 0x43C00000, mod 0x43C10000"
    puts "NOTE: the ADI DMAs now carry bytes, not IQ -- libiio will not return samples."
}
