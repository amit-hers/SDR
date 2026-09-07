# Deployment and Release

Everything needed to put a validated build on a Pluto+/LibreSDR board, and to
get one back afterwards. No Vivado knowledge is required for the normal
workflow — that is the point of the bundle.

## Operator quick start

```bash
tar --zstd -xf pluto-datalink-v1.3.0.tar.zst
cd pluto-datalink-v1.3.0
./scripts/flash.sh UNIT-A .
```

`UNIT-A` and `UNIT-B` are accepted as well as raw IPs. **The labels on the
boards do not match their addresses** — the unit whose USB serial says UNIT-B
answers on `192.168.2.1`, UNIT-A on `192.168.2.17`. The script maps the names
so nobody has to remember that.

Set `PLUTO_PW` if the board password is not `root`.

## The two deployment modes, and why the difference matters

| | what it does | survives a reboot |
|---|---|---|
| `flash.sh <dev> <dir>` | uploads the bitstream, loads it now, verifies | **no** |
| `flash.sh <dev> <dir> --persist` | writes the firmware image to `qspi-linux`, reboots, verifies | **yes** |

The board's rootfs is a **ramdisk**. A reboot rebuilds it from flash, discarding
`/lib/firmware`, `/tmp`, and anything else installed at runtime. So the default
mode deliberately does *not* reboot: rebooting would throw away the deployment
it just made and bring the board back on the stock PL, where every `0x43Cxxxxx`
access bus-errors in a way that looks exactly like dead hardware.

This is not hypothetical. A watchdog reset during a long test has silently
reverted a board to the stock PL more than once, and each time the symptom was
mistaken for an RF or interface fault before the bitstream was checked.

**`--persist` currently requires `boot/pluto.frm` in the bundle**, which
`build-release.sh` can only produce if `mkimage` (`u-boot-tools`) is installed
on the build machine. Without it the bundle omits the file and `--persist`
refuses with an explanation rather than half-flashing the board. See
*Power-on persistence* below.

## Creating a release

```bash
./release/build-release.sh v1.3.0-dev1 dev      # no tag needed
./release/build-release.sh v1.3.0     rc        # for hardware testing
git tag -a v1.3.0 -m "Validated Pluto datalink release v1.3.0"
./release/build-release.sh v1.3.0     release   # requires the tag
```

A `release` or `golden` build **refuses** to run unless the working tree is
clean and `HEAD` is at an annotated tag matching the requested version. A
permanent artifact that cannot be rebuilt from an immutable ref by someone else
is not a recovery image, whatever it is labelled.

Git provenance is read with `git rev-parse` and **there is no flag to override
it**. A release whose recorded revision does not match its contents is worse
than no metadata, because it will be believed.

## Marking a release permanent, and promoting to golden

```bash
./release/mark-validated.sh v1.3.0 "PER 0.00% over 12709 frames at 17.28 MS/s"
./release/promote-golden.sh v1.3.0
```

Building says the artifact assembled. `mark-validated.sh` says a human put it on
a board and watched it work. Only the second justifies permanent retention, and
`promote-golden.sh` refuses without it.

Promotion is explicit and never automatic — the newest release is the least
proven one, which is exactly backwards for a recovery image. The golden bundle
is copied to a second location (`SDR_GOLDEN_MIRROR`, default
`~/pluto-releases/golden`) and the mirror's checksum is re-verified, because a
recovery image on one disk is not a backup.

## Retention

| class | purpose | retention |
|---|---|---|
| `dev` | ordinary development build | 14 days |
| `rc` | release candidate for hardware testing | 90 days |
| `release` | hardware-validated | permanent |
| `golden` | known-good recovery image | permanent + mirrored |

```bash
./release/retention.sh --dry-run
./release/retention.sh
```

The class is read from the artifact's own `.class` file written at build time,
so pruning never depends on anyone remembering what a build was for. Anything
whose class cannot be determined is **kept** — the failure mode of a cleanup
script must be leaving too much, never too little.

## Verifying a deployment

`flash.sh` runs `verify.sh` automatically on success. To re-check at any time:

```bash
./scripts/verify.sh UNIT-A .
```

```
Release:             v1.3.0       PASS
FPGA image:          operating    PASS
FPGA magic:          0x5344524C   PASS
FPGA ABI:            3            PASS
Software ABI:        3            PASS
Register map:        3            PASS
IIO/AD936x:          ad9361-phy   PASS
Modem core:          0x00000004   PASS
Watchdog:            running      PASS

DEPLOYMENT RESULT: PASS
```

`verify.sh` checks the device against the **bundle's own manifest**, not against
constants compiled into the script, so an old bundle can still be verified years
later when the expected values have moved on.

## Recovering a failed deployment

Nothing is written to the device until all of these pass: bundle exists,
`manifest.json` exists, SHA-256 verifies, device reachable over SSH, hardware
identity matches, required artifacts present. A failure before that point has
changed nothing.

If a `--persist` deployment fails *after* the flash write, do **not** power the
board off — re-run the same command. `flashcp` is idempotent.

If the board no longer responds:

```bash
./scripts/rollback.sh UNIT-A            # finds the golden bundle
./scripts/rollback.sh UNIT-A /path/to/pluto-datalink-v1.2.0
```

Rollback simply re-runs the golden bundle's own `flash.sh --persist`. No special
path and no partial restore — the recovery route should be the best-tested one
in the system, not a separate one used only in emergencies.

If the board is unreachable over the network entirely, it can still be recovered
over USB DFU using `pluto.dfu` from the firmware tree; that path does not depend
on Linux running on the board at all.

## Version and ABI fields

Defined once in `release/versions.env`, compiled into the bitstream by
`fpga/bd/sdr_version_id.tcl`, and checked at runtime by
`fpga/scripts/fpga_abi_check.sh`.

The FPGA exposes them read-only at **`0x43C50000`**:

| offset | field | meaning |
|---|---|---|
| `0x00` | `MAGIC` | `0x5344524C`. Proves the window holds our identity block at all, rather than an unmapped address reading plausibly. Never changes. |
| `0x04` | `FPGA_VERSION` | `0x00MMmmpp`. Human-facing only — **never gate on it**, or a patch release could lock software out. |
| `0x08` | `FPGA_ABI_VERSION` | Bump when the *meaning* of a register changes: a field widens, a value is reinterpreted, a bit is repurposed. |
| `0x0C` | `REGISTER_MAP_VERSION` | Bump when a register *moves* or is added. |
| `0x10` | `BUILD_EPOCH` | UTC seconds, ties the PL to a manifest. |
| `0x14` | `GIT_SHA` | First 32 bits of the source commit. |

Why this exists: a mismatched bitstream does not fail loudly on its own. The
register reads still succeed — they just mean something else. That has already
happened here. Adding one HLS scalar shifted `bpsk_mode` from `0x18` to `0x24`,
and every script writing the old address carried on "working" while configuring
a different register entirely; the symptoms looked like RF problems and were
chased as such. Refusing to start is cheaper than any amount of debugging
downstream of a silent mismatch.

On mismatch the check prints and exits non-zero:

```
ERROR: FPGA/software incompatibility
FPGA ABI: 3
Software expects: 4
Refusing to start modem.
```

The identity block is on the CPU clock, not the modem clock, so it stays
readable when the modem cores are held in reset or stalled — which is exactly
when a version mismatch is most likely to be the cause of whatever is being
debugged.

## Power-on persistence

u-boot loads the PL at boot from `bitstream_image=system.bit.bin` inside the
firmware image on the `qspi-linux` partition (`mtd3`, 30 MB). That is the only
route to power-on persistence on this board:

* `/mnt/jffs2` (`mtd2`) is persistent but holds **700 KB free** — the bitstream
  is 2.5 MB, and 867 KB even gzipped. It is used for config, the release marker
  and `autorun.sh`, not for the bitstream.
* `qspi-fsbl-uboot` (`mtd0`) is **1 MB**, so it cannot hold a bitstream either;
  the 3.3 MB `BOOT.BIN` in the bundle is for SD-card boot, not QSPI.

So `--persist` writes a rebuilt `pluto.frm`. Building that needs `mkimage` from
`u-boot-tools` (`sudo apt-get install u-boot-tools`); without it, bundles ship
without `boot/pluto.frm` and `--persist` refuses cleanly.

**Verified on hardware.** UNIT-A was flashed with `--persist`, power-cycled, and
came up with the modem PL already loaded by u-boot — no script had run:

```
FPGA magic:          0x5344524C   PASS
FPGA ABI:            3            PASS
Register map:        3            PASS
DEPLOYMENT RESULT: PASS
```

### `--persist` resets the board's identity

This is the one thing to know before using it. The rootfs carries the board's
identity, so replacing it resets **hostname, USB serial, MAC, root password
(`analog`, not `root`) and IP address** to stock defaults. The stock IP is
`192.168.2.1`, which **collides with the other board** if both are attached.

It is not a failure, but it caught this script out once: UNIT-A deployed
perfectly and was reported dead because `flash.sh` was waiting on an address the
board no longer had. The wait now probes the stock address and both passwords
too, and says so explicitly when the board moves.

To restore the address, either:

* mount the board's mass-storage volume, set `ipaddr` in `config.txt`, and
  **eject it** — the board applies the change on unmount (it does nothing until
  then); or
* use the USB serial console, which works regardless of networking:

```bash
stty -F /dev/ttyACM1 115200 raw -echo
# log in as root / analog, then:
ip addr del 192.168.2.1/24 dev usb0
ip addr add 192.168.2.17/24 dev usb0
```

The board's own copy lives at `/opt/config.txt`.

Finally, **both host NICs sit in `192.168.2.0/24`**, so the kernel sends every
`192.168.2.x` packet out whichever route has the lower metric and one board
looks dead. Add a `/32` route per board:

```bash
sudo ip route replace 192.168.2.17/32 dev <nic> src 192.168.2.10 metric 50
```

`flash.sh` also installs `/mnt/jffs2/autorun.sh`, sourced at boot by
`/etc/init.d/S98autostart`. It re-raises the watchdog timeout to `-T 120` —
the stock `-T 10` is outrun by a multi-megabyte transfer or a `devmem` burst,
and has reset boards mid-test.

## CI

* `.github/workflows/ci.yml` — builds, runs the unit tests, parses every shell
  script, builds a `dev` bundle and **asserts that tampering with a file is
  detected**.
* `.github/workflows/release.yml` — on a `v*` tag, rebuilds the bundle from the
  tag and attaches it to a GitHub Release.

**Vivado cannot run on a GitHub runner**, and this project's licence is
node-locked to a USB NIC besides. The bitstream is therefore an *input* to the
release, not a product of it: build it on the licensed machine and commit it to
`fpga/prebuilt/system_top.bit` as part of preparing the tag. The manifest still
records `fpga_commit`, so the bitstream remains tied to a reviewable revision,
and `release.yml` fails with an explicit message if it is missing.
