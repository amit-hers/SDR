# Empirical AD9363 pin probe -- a throwaway diagnostic, not part of the modem.
#
# Every inference about this board's RX pin map has failed against hardware, so
# this measures instead. All 32 AD9363-interface balls are read as INPUTS by an
# axi_gpio; software samples the register repeatedly and reports which bits
# change. In CMOS mode the chip continuously drives DATA_CLK, RX_FRAME and 12
# data lines, so those bits vary while the rest sit still.
#
# PULLDOWN on every probe pin is what makes the result trustworthy: an undriven
# input floats and can look "active", which would produce exactly the false
# positive this exercise exists to avoid. With a pulldown, quiet means quiet.
#
# enable/txnrx/resetb are driven from constants so the AD9363 keeps streaming
# after the PL is reconfigured (those lines come from the PL and would
# otherwise drop, silencing the chip and making every pin look dead).
set part xc7z020clg400-2
set outdir /tmp/pin_probe
file delete -force $outdir
create_project probe $outdir -part $part -force

set pins {
  N20 P20 U18 U19
  Y18 V17 V20 R16 W18 V16
  Y19 V18 W20 R17 W19 W16
  N18 P19 Y16 Y17
  W14 T12 U14 T16 V12 V15
  Y14 U12 U15 U17 W13 W15
}
set names {
  rxclk_p rxclk_n rxfrm_p rxfrm_n
  rxdp0 rxdp1 rxdp2 rxdp3 rxdp4 rxdp5
  rxdn0 rxdn1 rxdn2 rxdn3 rxdn4 rxdn5
  txclk_p txclk_n txfrm_p txfrm_n
  txdp0 txdp1 txdp2 txdp3 txdp4 txdp5
  txdn0 txdn1 txdn2 txdn3 txdn4 txdn5
}

create_bd_design system
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
  -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps
set_property -dict [list CONFIG.PCW_USE_M_AXI_GP0 {1} CONFIG.PCW_EN_CLK0_PORT {1} \
  CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100.0}] $ps

set gpio [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio probe_gpio]
set_property -dict [list CONFIG.C_GPIO_WIDTH {32} CONFIG.C_ALL_INPUTS {1}] $gpio

# 32 probe inputs -> concat -> gpio
set cat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat probe_cat]
set_property CONFIG.NUM_PORTS {32} $cat
for {set i 0} {$i < 32} {incr i} {
  set nm [lindex $names $i]
  create_bd_port -dir I $nm
  connect_bd_net [get_bd_ports $nm] [get_bd_pins probe_cat/In$i]
}
connect_bd_net [get_bd_pins probe_cat/dout] [get_bd_pins probe_gpio/gpio_io_i]

# Hold the radio streaming: enable=1, resetb=1, txnrx=0, en_agc=0
set c1 [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant const1]
set_property -dict [list CONFIG.CONST_VAL {1} CONFIG.CONST_WIDTH {1}] $c1
set c0 [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant const0]
set_property -dict [list CONFIG.CONST_VAL {0} CONFIG.CONST_WIDTH {1}] $c0
foreach {p src} {enable const1 gpio_resetb const1 txnrx const0 gpio_en_agc const0} {
  create_bd_port -dir O $p
  connect_bd_net [get_bd_ports $p] [get_bd_pins $src/dout]
}

set sc [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axil]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $sc
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] [get_bd_intf_pins axil/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axil/M00_AXI] [get_bd_intf_pins probe_gpio/S_AXI]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins axil/aclk]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins probe_gpio/s_axi_aclk]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins ps7/M_AXI_GP0_ACLK]
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rstgen]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins rstgen/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rstgen/ext_reset_in]
connect_bd_net [get_bd_pins rstgen/peripheral_aresetn] [get_bd_pins axil/aresetn]
connect_bd_net [get_bd_pins rstgen/peripheral_aresetn] [get_bd_pins probe_gpio/s_axi_aresetn]

assign_bd_address
set seg [get_bd_addr_segs -quiet ps7/Data/SEG_probe_gpio_Reg]
if {[llength $seg]} { set_property offset 0x41200000 $seg ; set_property range 64K $seg }

validate_bd_design
save_bd_design
make_wrapper -files [get_files system.bd] -top
add_files -norecurse $outdir/probe.gen/sources_1/bd/system/hdl/system_wrapper.v
set_property top system_wrapper [current_fileset]

# constraints
set xdc $outdir/probe.xdc
set fh [open $xdc w]
for {set i 0} {$i < 32} {incr i} {
  puts $fh "set_property -dict {PACKAGE_PIN [lindex $pins $i] IOSTANDARD LVCMOS25 PULLTYPE PULLDOWN} \[get_ports [lindex $names $i]\]"
}
puts $fh "set_property -dict {PACKAGE_PIN R18 IOSTANDARD LVCMOS25} \[get_ports enable\]"
puts $fh "set_property -dict {PACKAGE_PIN P14 IOSTANDARD LVCMOS25} \[get_ports txnrx\]"
puts $fh "set_property -dict {PACKAGE_PIN N17 IOSTANDARD LVCMOS25} \[get_ports gpio_resetb\]"
puts $fh "set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS25} \[get_ports gpio_en_agc\]"
close $fh
add_files -fileset constrs_1 -norecurse $xdc

launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
puts "PROBE_BUILD_DONE: [get_property STATUS [get_runs impl_1]]"
