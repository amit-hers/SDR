# FPGA and recovery assets

These directories are development and board-recovery material, not part of the
normal host build. Use them only if you understand the target Pluto+ hardware,
Vivado toolchain, boot chain, and risk of rendering a device unbootable.

## FPGA sources

`fpga/` contains:

- HLS blocks for RSSI measurement, gain control, QPSK modem work, and sync
  detection;
- `fpga/bd/pluto_sdr_bd.tcl`, a Vivado block-diagram script;
- timing-synchronizer RTL and test bench;
- experimental analysis scripts and JTAG configuration.

Typical HLS entry points are the `hls_build.tcl` files:

```bash
vivado_hls -f fpga/hls/rssi_meter/hls_build.tcl
vivado_hls -f fpga/hls/gain_block/hls_build.tcl
vivado_hls -f fpga/hls/sync_detector/hls_build.tcl
vivado_hls -f fpga/hls/qpsk_modem/hls_build.tcl
```

Tool versions, part numbers, IP catalog paths, and address assignments are
encoded in the TCL and source files; inspect them before synthesis. The host
daemon currently streams through libiio and does not require these custom HLS
cores for its normal software pipeline.

`include/sdr/hardware/FPGARegs.hpp` describes an experimental memory-mapped
register interface. It accesses `/dev/mem`, requires elevated privileges, and
is useful only with a matching bitstream/address map.

## Recovery directory

`recovery/` includes OpenOCD/JTAG configurations, TCL flash operations, a
small QSPI flasher program, linker scripts, and shell wrappers. It also
contains a prebuilt `qspi_flasher.elf`; treat prebuilt binaries as
hardware/version-specific and rebuild or verify provenance where possible.

The `tezuka-plutoplus-v0.3.5-7cf6171/` directory contains board firmware and
boot artifacts. Do not flash them merely to run the host daemon.

Before any recovery operation:

1. Identify the exact board revision, FPGA part, flash geometry, and JTAG
   adapter.
2. Read the selected script and confirm every address and input filename.
3. Back up accessible environment and flash regions.
4. Prefer diagnostic/read/verify operations before erase or program actions.
5. Maintain a known-good vendor recovery path and stable power.

Recovery and flash programming are destructive operations. No recovery script
is invoked by the standard CMake build, daemon, dependency installer, or
monitor.
