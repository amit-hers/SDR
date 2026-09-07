# FPGA and recovery assets

These directories are development and board-recovery material, not part of the
normal host build. Use them only if you understand the target Pluto+ hardware,
Vivado toolchain, boot chain, and risk of rendering a device unbootable.

> For the modem that is actually built and measured from these sources — its
> throughput, register map, bring-up order and measurement tools — see
> [Fabric modem](fabric-modem.md). For putting a build on a board, see
> [Deployment](DEPLOYMENT.md); the normal deployment workflow needs no Vivado
> knowledge at all.

## Which toolchain

**Vivado/Vitis 2025.1 builds the integrated design.** The ADI HDL tree expects
it, and it passes the version check unaided; 2026.1 needs
`ADI_IGNORE_VERSION_CHECK=1` and is not what the hardware-validated bitstreams
were built with. 2026.1 is still used for `bootgen`, which the release builder
calls to assemble `BOOT.BIN`.

The licence is **node-locked to a USB NIC** (`enx00e0226dc9b7`). `set_part`
fails when that adapter is absent, which reads as an unsupported part rather
than a licence problem. This is also why the bitstream cannot be built in CI and
is committed to `fpga/prebuilt/` as an input to a release — see
[Deployment](DEPLOYMENT.md#ci).

The full-design build is driven from the ADI project, not from this repository:

```bash
export PATH=/path/to/Vivado/2025.1/Vivado/bin:$PATH
cd adi-hdl/projects/libre && make clean && make
```

`fpga/libre/` holds a snapshot of the four files that turn a stock ADI `libre`
checkout into this build; `fpga/bd/sdr_insert.tcl` is what splices the modem in.
**The Makefile does not track any file in this repository**, so `make` alone can
return 0 having rebuilt nothing — clean first, and check the `.bit` mtime rather
than the exit status.

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
boot artifacts: `BOOT.bin`, `uImage`, `devicetree.dtb`, `uramdisk.image.gz` and
the packaged `pluto.frm`/`.dfu`. Do not flash them merely to run the host daemon.

`release/build-release.sh` consumes them as release inputs, and
`release/make-frm.sh` rebuilds `pluto.frm` with the project's own bitstream so
the modem PL loads at power-on — u-boot loads the PL from that image's `fpga`
sub-image before the kernel. The kernel, device tree and ramdisk are carried
across unchanged.

Note that flashing the firmware **replaces the rootfs, and the rootfs carries
the board's identity**: hostname, USB serial, MAC, root password and IP all
return to stock defaults, and the stock IP collides with a second attached
board. [Deployment](DEPLOYMENT.md) documents the recovery routes.

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
