
source ../../scripts/adi_env.tcl
source $ad_hdl_dir/projects/scripts/adi_project_xilinx.tcl
source $ad_hdl_dir/projects/scripts/adi_board.tcl

adi_project_create libre 0 {} "xc7z020clg400-2"

adi_project_files libre [list \
  "system_top.v" \
  "system_constr.xdc" \
  "$ad_hdl_dir/library/common/ad_iobuf.v" \
  "/home/amither/Documents/SDR/fpga/bd/sdr_clock_groups.xdc"]

set_property is_enabled false [get_files  *system_sys_ps7_0.xdc]
# Implementation-only: the modem clock does not exist at synthesis time, where
# every constraint would silently no-op instead of failing loudly.
set_property used_in_synthesis false [get_files *sdr_clock_groups.xdc]
adi_project_run libre
source $ad_hdl_dir/library/axi_ad9361/axi_ad9361_delay.tcl

