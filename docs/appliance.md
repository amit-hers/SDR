# Standalone appliance mode

Appliance mode turns a pair of boards into a transparent Ethernet-over-RF cable.
Plug a PC or camera into one board's RJ45 and a PC or drone into the other's,
power both, and frames cross. Nothing is installed on the attached computers,
nobody logs in, and no SDR software runs on either end.

---

## 1. How it fits together

```
  PC / camera                                               PC / drone
      |                                                          |
   RJ45 (eth0)                                              RJ45 (eth0)
      |                                                          |
 +----+------------------+                        +--------------+----------+
 |  UNIT-A               |                        |  UNIT-B                 |
 |                       |                        |                         |
 |  sdr_bridge           |                        |  sdr_bridge             |
 |   AF_PACKET on eth0   |                        |   AF_PACKET on eth0     |
 |   frame + scramble    |                        |   deframe + descramble  |
 |        |              |                        |        ^                |
 |   /dev/iio:device2    |                        |   /dev/iio:device3      |
 |        |              |                        |        |                |
 |  PL: qpsk_mod         |   ~~~~~ 434 MHz ~~~~>  |  PL: qpsk_demod         |
 |  AD9363 TX            |                        |  AD9363 RX              |
 +-----------------------+                        +-------------------------+
      |                                                          |
   usb0 (management, 192.168.2.x) -- not part of the data path
```

The split that matters: **modulation and demodulation happen in the FPGA
fabric**, not in software. `sdr_bridge` never sees I/Q. It captures Ethernet
frames, wraps them in the link's frame format, and hands a **byte stream** to the
PL through libiio. The ARM only does framing and deframing, which is why a
500 MHz core keeps up with 7.85 Mbit/s of goodput at ~3.5 % CPU.

Two interfaces, two jobs, and confusing them causes trouble:

| Interface | Driver | Role |
|---|---|---|
| `eth0` | `macb` | The RJ45. The **data path**. No IP address — it is a wire, not a host. |
| `usb0` | gadget | Management over USB, `192.168.2.x`. Not bridged. |

Manage the board over `usb0`. If you put management on `eth0` you are
administering the same wire the bridge is reconfiguring and capturing.

---

## 2. What runs at boot

```
S98autostart
  └─ /mnt/jffs2/autorun.sh          (loads the FPGA bitstream, relaxes the watchdog)
       └─ /mnt/jffs2/appliance_start.sh   &
            ├─ wait for IIO devices BY NAME (up to 60 s)
            ├─ tools/tx_fabric.sh   <rate> <freq> <diff>
            ├─ tools/rx_framed.sh   <rate>
            ├─ assert mod_en and dem_en are both 1
            └─ exec sdr_bridge --raw-eth eth0 --stats 30
```

Measured on a cold boot: IIO devices present at 15 s, modem configured at 18 s,
bridge serving at 18 s.

**The order is not optional.** `sdr_bridge` does not configure the fabric modem
or the AD9363 — it only opens the IIO devices and frames what arrives. Started
against an unconfigured datapath it spawns `iio_writedev`/`iio_readdev`, both go
defunct immediately, and every write increments the TX error counter while the
packet counter stays frozen: error counts past 2300 with TX stuck at 9 packets,
and nothing saying why. So the script verifies `mod_en` and `dem_en` both read
`0x00000001` and refuses to start the bridge otherwise.

| Register | Meaning |
|---|---|
| `0x43C10010` | modulator enable (`mod_en`) |
| `0x43C00010` | demodulator enable (`dem_en`) |
| `0x43C50000` | FPGA identity block |

---

## 3. Files on the board

Everything persistent lives in `/mnt/jffs2`, an **896 KB** jffs2 on `mtd2`. It is
the only writable storage that survives a reboot — there is no SD card and no
eMMC. `/root` and `/tmp` are ramdisk and are gone after a power cycle.

| Path | What |
|---|---|
| `/mnt/jffs2/sdr_bridge` | the bridge binary (~120 KB) |
| `/mnt/jffs2/appliance_start.sh` | the startup script |
| `/mnt/jffs2/bridge.conf` | configuration; **its absence disables appliance mode** |
| `/mnt/jffs2/tools/` | the modem bring-up scripts |
| `/mnt/jffs2/autorun.sh` | sourced at boot; calls the above |
| `/tmp/appliance.log` | the boot log (ramdisk — read it before rebooting) |

### The binary must be dynamically linked

A statically linked build is 542 KB. It does not reliably fit. jffs2 keeps
obsolete nodes until it can garbage-collect them, so rewriting a large file
leaves far less free space than the directory listing suggests, and the write
then **stops short and leaves a truncated binary with no error** — `cat` and `cp`
both report success. This produced a 315 KB and then a 249 KB "binary".

Linking glibc dynamically gives 120 KB. `libstdc++` stays static because the
rootfs does not ship it; `libc.so.6` (Buildroot glibc 2.41) and
`ld-linux-armhf.so.3` are both present. `fpga/tools/build_bridge.sh` does this.

Always verify a deployed binary by checksum, never by file size alone:

```bash
md5sum /mnt/jffs2/sdr_bridge     # must match the host copy
```

---

## 4. Configuration

`/mnt/jffs2/bridge.conf`:

```sh
MODE=raw-eth            # raw-eth (transparent L2) or tun (routed L3)
IFACE=eth0              # the RJ45
SAMPLE_RATE=3840000
FREQUENCY=434000000
DIFF_MODE=1             # differential QPSK
STATS_S=30              # statistics interval, seconds
```

For `MODE=tun`, set `LOCAL_IP`, `PEER_IP` and optionally `TUN_IFACE` instead of
`IFACE`.

Both ends must agree on sample rate, frequency and differential mode.

---

## 5. Installing it

```bash
IP=192.168.2.1    # over usb0
scp_or_cat release/appliance/appliance_start.sh  root@$IP:/mnt/jffs2/
scp_or_cat release/appliance/bridge.conf.example root@$IP:/mnt/jffs2/bridge.conf
scp_or_cat build/sdr_bridge                      root@$IP:/mnt/jffs2/sdr_bridge
```

`scp` is unavailable on the board — there is no `sftp-server`. Pipe over ssh:

```bash
ssh root@$IP 'cat > /mnt/jffs2/sdr_bridge; chmod +x /mnt/jffs2/sdr_bridge; sync' < build/sdr_bridge
```

Free space first if you are replacing a file: `rm` it, `sync`, and confirm the
space actually came back before writing (see the truncation trap above).

Then hook it into boot, once:

```sh
printf '\n[ -x /mnt/jffs2/appliance_start.sh ] && /mnt/jffs2/appliance_start.sh >/dev/null 2>&1 &\n' \
  >> /mnt/jffs2/autorun.sh
```

Power-cycle and read `/tmp/appliance.log`.

---

## 6. Running it by hand

```bash
/mnt/jffs2/tools/tx_fabric.sh 3840000 434000000 1
/mnt/jffs2/tools/rx_framed.sh 3840000
/mnt/jffs2/sdr_bridge --raw-eth eth0 --stats 30
```

Reading the statistics line:

```
tx 0 pkts / 0 B (idle 0, err 0) | rx 196 dma, 0 frames, 0 B (crcerr 0, dup 0, ctrl 0) | offsets 0/0/0/0
cpu decode 3.3% now / 3.5% avg | rx gap max 321918 us, short 0 | tx stall 0 ms | oversize 0
```

- `rx ... dma` climbing with `frames 0` is normal when no peer is transmitting —
  the receive path is alive and finding no frames.
- `err` climbing with `tx` frozen means the modem was not configured first.
- `oversize` counts frames larger than the radio's payload limit.
- `crcerr` is the real link-quality number.

---

## 7. Pitfalls

**The bridge must not capture its own injections.** An `ETH_P_ALL` AF_PACKET
socket is also delivered the frames its own host transmits. Without suppression,
every packet decoded off the radio and injected onto the wire is captured again
and retransmitted — between two units that is an unconditional loop that needs
no broadcast traffic to start and does not decay, because each lap is
regenerated at full power by the modem. `sdr_bridge` sets
`PACKET_IGNORE_OUTGOING` (kernel 4.20+). If you see the warning that it could
not, expect a storm.

**MTU must be set with the link down.** `macb` rejects `SIOCSIFMTU` on a running
interface with `EBUSY`, so `ip link set eth0 up mtu 1400` silently leaves it at
1500 — and if the code then prints the *requested* value, the log claims 1400
while the wire carries 1500. Full-size 1514-byte frames, which is most of a
video feed, are counted oversize and dropped. The bridge now sets it with the
link down and reports what `/sys/class/net/eth0/mtu` actually says.

**Attached equipment should use an MTU of 1400 or less**, or its full-size
frames will not fit the radio payload.

**Never start two bridges on one interface.** Both capture promiscuously and
both transmit, so every frame crosses the radio twice and the far end sees
duplicates of traffic that was never duplicated. The startup script refuses a
second instance, identifying processes by `/proc/PID/exe`.

**Do not identify processes by matching a command-line pattern.** `pkill -f
sdr_bridge` matches the shell that is running it, including an ssh command line
containing that text, and kills the session instead of the bridge. Use
`pkill -x`, or resolve `/proc/PID/exe`.

**Wait for the IIO devices by name, not by index.** At boot the appliance starts
as soon as the bitstream loads, which is before the AD9363 and the fabric DMA
cores finish probing. The indices are also not stable — the stock rootfs has
been seen to renumber them, and a script holding `device3` eventually addresses
the wrong core. Wait for `ad9361-phy`, `cf-ad9361-dds-core-lpc` and
`cf-ad9361-lpc`.

**A silent transmitter is usually `EBUSY`, not RF.** The IIO character device is
single-open. If another process (`maia_sdr` in the stock Pluto+ firmware, for
one) holds the buffers, the modem cannot open them. Check elapsed feed time; a
process stuck in D-state needs a power cycle.

**Raw `write()` to `/dev/iio:*` does not program the DMA on 6.12.** Use libiio or
`iio_writedev`. This, not RF, is why a transmitter appears dead.

**`TX_LO_powerdown` may be set by vendor scripts.** Some stock Pluto+ images
(tezuka's DATV scripts) write `TX_LO_powerdown=1` on every `ensm_mode=rx`. It is
register `0x051` bit 4. Both radios silent with good receivers is this, not RF
and not cabling.

**Storage is 896 KB and fragments.** Keep the binary small, delete before
writing, `sync`, and verify by checksum. A reboot lets jffs2 garbage-collect and
frees space that `rm` alone does not.

---

## 8. Verifying a unit

```bash
# management is on usb0; eth0 carries no IP
ip -o -4 addr show                    # expect usb0 only

# the datapath is configured
devmem 0x43C10010 32                  # mod_en -> 0x00000001
devmem 0x43C00010 32                  # dem_en -> 0x00000001

# exactly one bridge, and it is promiscuous at the right MTU
cat /sys/class/net/eth0/mtu           # 1400
cat /sys/class/net/eth0/flags         # 0x1103 -> promiscuous + up
tail -n +2 /proc/net/packet | wc -l   # 1

# and the children are alive, not defunct
ps w | grep -E 'iio_readdev|iio_writedev'
```
