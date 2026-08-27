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

Build the HLS cores with the wrapper, which sources the Vitis environment,
points HLS at Vivado for IP packaging, and warns about the two things a bare
install is missing:

```bash
fpga/hls/build.sh                 # every core
fpga/hls/build.sh qpsk_modem      # just one
```

`vivado_hls` was retired; 2026.1 runs HLS through `vitis-run --mode hls`. To
drive it directly:

```bash
source /home/amither/Documents/vivado/2026.1/Vitis/settings64.sh
cd fpga/hls/rssi_meter && vitis-run --mode hls --tcl hls_build.tcl
```

Part and clock come from `fpga/hls/common.tcl` and can be overridden per run:

| variable | default | note |
|---|---|---|
| `SDR_HLS_PART` | `xc7z020clg400-2` | the HamGeek Pluto+. A base ADALM-Pluto is `xc7z010clg400-1` and is **not** interchangeable |
| `SDR_HLS_CLOCK` | `5` | ns, i.e. 200 MHz |
| `XILINX_VITIS` | the 2026.1 install | point at another toolchain |

### Two things a stock install still needs

Verified on 2026.1: the scripts run, the tool executes every command through
`set_part`, and then stops on both of these. Neither is a project problem.

1. **Zynq-7000 device data.** `data/parts/xilinx/` ships artix7, kintex7,
   spartan*, versal and virtex* but no zynq, so `xc7z020clg400-2` is unknown.
   Re-run the AMD installer and tick Devices > SoCs > Zynq-7000.
2. **A licence.** `ERROR: Vivado Design Suite cannot be launched because a
   valid license was not found.` Zynq-7020 is covered by the free Vivado ML
   Standard licence; generate one and place it at `~/.Xilinx/Xilinx.lic`, or
   set `XILINXD_LICENSE_FILE`.

`build.sh` checks for both up front and says so in one sentence each, because
otherwise they surface as `[HLS 200-1023] Part ... is not supported`, which
reads like a bad part number rather than a missing install.

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
