# AD9363 RX bit-order solver -- second diagnostic design.
#
# Established facts this builds on (measured, not inferred, by probe.tcl):
#   DATA_CLK  = N20   RX_FRAME = U18
#   12 data balls = Y18 V17 V20 R16 W18 V16 Y19 V18 W20 R17 W19 W16
#   Only the P-halves are driven -> the port really is single-ended CMOS.
# The one remaining unknown is which ball carries which bit. This captures the
# lanes synchronously on DATA_CLK into a BRAM while the AD9363 runs BIST PRBS,
# so the permutation is recovered from data.
#
# LANE ORDER BELOW IS THE MEASUREMENT'S COORDINATE SYSTEM, not a claim about
# bit weight: captured word bit i == package pin rx_pins[i]. The decode step
# maps lane -> AD9363 bit.
set part   xc7z020clg400-2
set outdir /tmp/pn_probe
set srcdir [file dirname [file normalize [info script]]]
file delete -force $outdir
create_project pnprobe $outdir -part $part -force

set rx_pins  {Y18 V17 V20 R16 W18 V16 Y19 V18 W20 R17 W19 W16}
set clk_pin  N20
set frm_pin  U18

create_bd_design system
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
  -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps
set_property -dict [list CONFIG.PCW_USE_M_AXI_GP0 {1} CONFIG.PCW_EN_CLK0_PORT {1} \
  CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100.0} \
  CONFIG.PCW_SPI0_PERIPHERAL_ENABLE {1} CONFIG.PCW_SPI0_SPI0_IO {EMIO} \
  CONFIG.PCW_SPI0_GRP_SS0_ENABLE {1}] $ps

# SPI0 EMIO -> AD9363. Without this the probe bitstream silently breaks SPI:
# debugfs writes still 'succeed' (the driver never reads back), so BIST would
# look enabled while the chip actually kept emitting plain ADC noise.
set k0 [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant spi_lo]
set_property -dict [list CONFIG.CONST_VAL {0} CONFIG.CONST_WIDTH {1}] $k0
set k1 [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant spi_hi]
set_property -dict [list CONFIG.CONST_VAL {1} CONFIG.CONST_WIDTH {1}] $k1
connect_bd_net [get_bd_pins spi_lo/dout] [get_bd_pins ps7/SPI0_SCLK_I]
connect_bd_net [get_bd_pins spi_lo/dout] [get_bd_pins ps7/SPI0_MOSI_I]
connect_bd_net [get_bd_pins spi_hi/dout] [get_bd_pins ps7/SPI0_SS_I]
make_bd_pins_external -name spi_clk  [get_bd_pins ps7/SPI0_SCLK_O]
make_bd_pins_external -name spi_mosi [get_bd_pins ps7/SPI0_MOSI_O]
make_bd_pins_external -name spi_csn  [get_bd_pins ps7/SPI0_SS_O]
make_bd_pins_external -name spi_miso [get_bd_pins ps7/SPI0_MISO_I]

# control/status: ch1 out = arm, ch2 in = done
set gp [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio ctrl]
set_property -dict [list CONFIG.C_IS_DUAL {1} CONFIG.C_ALL_OUTPUTS {1} \
  CONFIG.C_ALL_INPUTS_2 {1} CONFIG.C_GPIO_WIDTH {32} CONFIG.C_GPIO2_WIDTH {32}] $gp

# capture buffer: port A -> AXI (PS reads), port B -> capture logic (DATA_CLK)
set bc [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl bramctrl]
set_property -dict [list CONFIG.DATA_WIDTH {32} CONFIG.SINGLE_PORT_BRAM {1}] $bc
set mem [create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen bram]
set_property -dict [list CONFIG.Memory_Type {True_Dual_Port_RAM} \
  CONFIG.use_bram_block {BRAM_Controller} CONFIG.EN_SAFETY_CKT {false}] $mem
connect_bd_intf_net [get_bd_intf_pins bramctrl/BRAM_PORTA] [get_bd_intf_pins bram/BRAM_PORTA]
make_bd_intf_pins_external -name BRAM_PORTB [get_bd_intf_pins bram/BRAM_PORTB]
make_bd_pins_external -name arm  [get_bd_pins ctrl/gpio_io_o]
make_bd_pins_external -name stat [get_bd_pins ctrl/gpio2_io_i]

set sc [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axil]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {2}] $sc
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0]  [get_bd_intf_pins axil/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axil/M00_AXI]   [get_bd_intf_pins ctrl/S_AXI]
connect_bd_intf_net [get_bd_intf_pins axil/M01_AXI]   [get_bd_intf_pins bramctrl/S_AXI]
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rstgen]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins rstgen/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rstgen/ext_reset_in]
foreach p {axil/aclk ctrl/s_axi_aclk bramctrl/s_axi_aclk ps7/M_AXI_GP0_ACLK} {
  connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins $p]
}
foreach p {axil/aresetn ctrl/s_axi_aresetn bramctrl/s_axi_aresetn} {
  connect_bd_net [get_bd_pins rstgen/peripheral_aresetn] [get_bd_pins $p]
}

assign_bd_address
set s [get_bd_addr_segs -quiet ps7/Data/SEG_ctrl_Reg]
if {[llength $s]} { set_property offset 0x41200000 $s ; set_property range 64K $s }
set s [get_bd_addr_segs -quiet ps7/Data/SEG_bramctrl_Mem0]
if {[llength $s]} { set_property offset 0x40000000 $s ; set_property range 8K $s }

validate_bd_design
save_bd_design
make_wrapper -files [get_files system.bd] -top
set wrap $outdir/pnprobe.gen/sources_1/bd/system/hdl/system_wrapper.v
add_files -norecurse $wrap

# ---- Discover the wrapper's real port names/widths rather than assuming them.
set fh [open $wrap r]; set txt [read $fh]; close $fh
set W [dict create]
foreach ln [split $txt "\n"] {
  if {[regexp {^\s*(input|output|inout)\s+(?:wire\s+)?(?:\[(\d+)\s*:\s*(\d+)\]\s*)?(\w+)} $ln -> d hi lo nm]} {
    dict set W $nm [expr {$hi eq "" ? 1 : $hi - $lo + 1}]
  }
}
proc findport {W pat} {
  dict for {k v} $W { if {[regexp $pat $k]} { return [list $k $v] } }
  error "PORT_NOT_FOUND: $pat"
}
lassign [findport $W {BRAM_PORTB.*addr}] p_addr w_addr
lassign [findport $W {BRAM_PORTB.*clk}]  p_clk  w_clk
lassign [findport $W {BRAM_PORTB.*din}]  p_din  w_din
lassign [findport $W {BRAM_PORTB.*_en}]  p_en   w_en
lassign [findport $W {BRAM_PORTB.*rst}]  p_rst  w_rst
lassign [findport $W {BRAM_PORTB.*we}]   p_we   w_we
lassign [findport $W {^arm}]             p_arm  w_arm
lassign [findport $W {^stat}]            p_stat w_stat
lassign [findport $W {^spi_clk}]         p_sclk w_sclk
lassign [findport $W {^spi_mosi}]        p_smo  w_smo
lassign [findport $W {^spi_csn}]         p_scs  w_scs
lassign [findport $W {^spi_miso}]        p_smi  w_smi
puts "WRAPPER PORTS: addr=$p_addr\[$w_addr\] din=$p_din\[$w_din\] we=$p_we\[$w_we\] arm=$p_arm\[$w_arm\] stat=$p_stat\[$w_stat\]"

# ---- Emit the top level against those discovered widths.
set top $outdir/capture_top.v
set fh [open $top w]
puts $fh "// generated by pn_probe.tcl -- widths taken from the BD wrapper"
puts $fh "module capture_top ("
puts $fh "  inout \[14:0\] DDR_addr, inout \[2:0\] DDR_ba,"
puts $fh "  inout DDR_cas_n, inout DDR_ck_n, inout DDR_ck_p, inout DDR_cke,"
puts $fh "  inout DDR_cs_n, inout \[3:0\] DDR_dm, inout \[31:0\] DDR_dq,"
puts $fh "  inout \[3:0\] DDR_dqs_n, inout \[3:0\] DDR_dqs_p, inout DDR_odt,"
puts $fh "  inout DDR_ras_n, inout DDR_reset_n, inout DDR_we_n,"
puts $fh "  inout FIXED_IO_ddr_vrn, inout FIXED_IO_ddr_vrp,"
puts $fh "  inout \[53:0\] FIXED_IO_mio, inout FIXED_IO_ps_clk,"
puts $fh "  inout FIXED_IO_ps_porb, inout FIXED_IO_ps_srstb,"
puts $fh "  input  rx_clk_in, input rx_frame_in, input \[11:0\] rx_data_in,"
puts $fh "  output enable, output txnrx, output gpio_resetb, output gpio_en_agc,"
puts $fh "  output spi_clk, output spi_mosi, output spi_csn, input spi_miso"
puts $fh ");"
puts $fh "  // keep the AD9363 streaming after PL reconfiguration"
puts $fh "  assign enable = 1'b1; assign gpio_resetb = 1'b1;"
puts $fh "  assign txnrx  = 1'b0; assign gpio_en_agc = 1'b0;"
puts $fh "  wire clk_ib, clk; wire \[$w_arm-1:0\] arm_w; wire cap_we; wire cap_done;"
puts $fh "  wire \[$w_addr-1:0\] cap_addr; wire \[31:0\] cap_din;"
puts $fh "  IBUF ib (.I(rx_clk_in), .O(clk_ib));"
puts $fh "  BUFG bg (.I(clk_ib), .O(clk));"
puts $fh "  pn_capture #(.DEPTH_LOG2(10), .ADDR_W($w_addr)) cap ("
puts $fh "    .clk(clk), .arm(arm_w\[0\]), .frame(rx_frame_in), .data(rx_data_in),"
puts $fh "    .we(cap_we), .addr(cap_addr), .din(cap_din), .done(cap_done));"
puts $fh "  system_wrapper sys ("
puts $fh "    .DDR_addr(DDR_addr), .DDR_ba(DDR_ba), .DDR_cas_n(DDR_cas_n),"
puts $fh "    .DDR_ck_n(DDR_ck_n), .DDR_ck_p(DDR_ck_p), .DDR_cke(DDR_cke),"
puts $fh "    .DDR_cs_n(DDR_cs_n), .DDR_dm(DDR_dm), .DDR_dq(DDR_dq),"
puts $fh "    .DDR_dqs_n(DDR_dqs_n), .DDR_dqs_p(DDR_dqs_p), .DDR_odt(DDR_odt),"
puts $fh "    .DDR_ras_n(DDR_ras_n), .DDR_reset_n(DDR_reset_n), .DDR_we_n(DDR_we_n),"
puts $fh "    .FIXED_IO_ddr_vrn(FIXED_IO_ddr_vrn), .FIXED_IO_ddr_vrp(FIXED_IO_ddr_vrp),"
puts $fh "    .FIXED_IO_mio(FIXED_IO_mio), .FIXED_IO_ps_clk(FIXED_IO_ps_clk),"
puts $fh "    .FIXED_IO_ps_porb(FIXED_IO_ps_porb), .FIXED_IO_ps_srstb(FIXED_IO_ps_srstb),"
puts $fh "    .${p_arm}(arm_w), .${p_stat}({[expr {$w_stat-1}]'d0, cap_done}),"
puts $fh "    .${p_clk}(clk), .${p_rst}(1'b0), .${p_en}(1'b1),"
puts $fh "    .${p_we}({$w_we\{cap_we\}}), .${p_addr}(cap_addr), .${p_din}(cap_din),"
puts $fh "    .${p_sclk}(spi_clk), .${p_smo}(spi_mosi), .${p_scs}(spi_csn), .${p_smi}(spi_miso));"
puts $fh "endmodule"
close $fh
add_files -norecurse [list $top $srcdir/pn_capture.v]
set_property top capture_top [current_fileset]

set xdc $outdir/pn.xdc
set fh [open $xdc w]
puts $fh "set_property -dict {PACKAGE_PIN $clk_pin IOSTANDARD LVCMOS25} \[get_ports rx_clk_in\]"
puts $fh "set_property -dict {PACKAGE_PIN $frm_pin IOSTANDARD LVCMOS25} \[get_ports rx_frame_in\]"
for {set i 0} {$i < 12} {incr i} {
  puts $fh "set_property -dict {PACKAGE_PIN [lindex $rx_pins $i] IOSTANDARD LVCMOS25} \[get_ports {rx_data_in\[$i\]}\]"
}
foreach {p pin} {enable R18 txnrx P14 gpio_resetb N17 gpio_en_agc P16 \
                 spi_clk R14 spi_mosi P15 spi_miso R19} {
  puts $fh "set_property -dict {PACKAGE_PIN $pin IOSTANDARD LVCMOS25} \[get_ports $p\]"
}
# DATA_CLK is unknown but well under 100 MHz; constrain conservatively fast.
puts $fh "set_property -dict {PACKAGE_PIN P18 IOSTANDARD LVCMOS25 PULLTYPE PULLUP} \[get_ports spi_csn\]"
puts $fh "create_clock -period 10.000 -name data_clk \[get_ports rx_clk_in\]"
puts $fh "set_clock_groups -asynchronous -group \[get_clocks data_clk\] \\"
puts $fh "  -group \[get_clocks -include_generated_clocks clk_fpga_0\]"
close $fh
add_files -fileset constrs_1 -norecurse $xdc

launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
puts "PN_PROBE_BUILD: [get_property STATUS [get_runs impl_1]]"
