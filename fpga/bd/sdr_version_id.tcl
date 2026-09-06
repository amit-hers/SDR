# sdr_version_id.tcl -- read-only identity block. Source AFTER sdr_insert.tcl.
#
# Lets software refuse to run against a bitstream it does not understand. Every
# register in this design is addressed by a hand-maintained offset and those
# offsets have moved before -- adding one HLS scalar once shifted bpsk_mode from
# 0x18 to 0x24 and broke every script writing the old address. A stale bitstream
# announces nothing: the reads still succeed, they just mean something else.
#
# The version values are stamped from the ENVIRONMENT at build time, not edited
# here, so a release artifact cannot claim a provenance its sources do not have.
# See release/build-release.sh, which exports them from the git tag.
puts "sdr_version_id: identity block at 0x43C50000"

set _magic  0x5344524C
set _ver    [expr {[info exists ::env(SDR_FPGA_VERSION)] ? $::env(SDR_FPGA_VERSION) : 0x00010000}]
set _abi    [expr {[info exists ::env(SDR_FPGA_ABI)]     ? $::env(SDR_FPGA_ABI)     : 1}]
set _regmap [expr {[info exists ::env(SDR_REGMAP_VER)]   ? $::env(SDR_REGMAP_VER)   : 1}]
set _epoch  [expr {[info exists ::env(SDR_BUILD_EPOCH)]  ? $::env(SDR_BUILD_EPOCH)  : 0}]
set _sha    [expr {[info exists ::env(SDR_GIT_SHA32)]    ? $::env(SDR_GIT_SHA32)    : 0}]

create_bd_cell -type module -reference axi_version_id version_id
set_property -dict [list \
  CONFIG.MAGIC        $_magic \
  CONFIG.FPGA_VERSION $_ver   \
  CONFIG.FPGA_ABI     $_abi   \
  CONFIG.REGMAP_VER   $_regmap \
  CONFIG.BUILD_EPOCH  $_epoch \
  CONFIG.GIT_SHA      $_sha   \
] [get_bd_cells version_id]

# Same clock and reset as the rest of the CPU-facing peripherals. Deliberately
# NOT the modem clock: the identity must stay readable when the modem cores are
# held in reset or stalled, which is exactly when a mismatch is most likely to
# be the cause of whatever is being debugged.
ad_connect sys_ps7/FCLK_CLK0    version_id/s_axi_aclk
ad_connect sys_rstgen/peripheral_aresetn version_id/s_axi_aresetn

# 0x43C50000: the next free window after the dac pin probe at 0x43C40000.
ad_cpu_interconnect 0x43C50000 version_id

puts "sdr_version_id: MAGIC=$_magic VERSION=$_ver ABI=$_abi REGMAP=$_regmap"
