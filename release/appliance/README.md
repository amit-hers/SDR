# Appliance startup

`appliance_start.sh` brings the radio up unattended: modem first, then the
bridge. `bridge.conf.example` documents the configuration; copy it to
`/mnt/jffs2/bridge.conf` to arm autostart. Absent that file the radio boots and
does nothing, deliberately -- a misconfiguration should not be able to leave a
board unreachable.

## Findings this encodes

**sdr_bridge does not configure the modem or the AD936x.** It opens the IIO
devices and frames what arrives; it does not touch the fabric registers. Started
against an unconfigured datapath it spawns iio_writedev/iio_readdev, both go
defunct at once, and every write then increments the tx error counter while the
packet counter stays frozen -- measured err past 2300 with tx stuck at 9, and no
message saying why. The script therefore runs the bring-up first and verifies
`mod_en`/`dem_en` actually read 1 before handing over, so a bring-up failure is
reported at its cause instead of as unexplained bridge errors later.

**jffs2 has no headroom.** The partition is 896 KB and the bridge binary alone is
734 KB. An update must overwrite in place; staging a second copy alongside it
fills the filesystem and truncates the upload at 64 KB free. Verify the sha256
on the board after any deploy.

## Loopback PER is the packet boundary, not the radio

Measured over AD9361 digital loopback, 4 MiB capture, 128 DMA packets:

    packets 128, anchored 128 (100.0%)
    capturable 3168, decoded 3040, MISSING 128 -> PER 4.04%
    CRC failures 32, payload mismatches 0, duty cycle 100%

MISSING equals the packet count exactly: one frame lost per DMA packet, the one
straddling the boundary. 3168/128 = 24.75 frames per packet, and 1/24.75 =
4.04%, matching the 3.9% that `fpga/bd/sdr_insert.tcl` already predicts for
PKT_BYTES 32768.

So this is the documented design point, not a defect, and PKT_BYTES is already
at its knee -- 65536 buys 0.15 Mbit/s for twice the buffering. Frames landing
wholly inside a packet decode at 99.85%.

Note the bytes are genuinely LOST at DMA re-arm (measured median 2 B, p90 3 B),
not merely scoped out by the per-packet deframer. Carrying deframer state across
boundaries therefore cannot recover them -- the straddling frame is truncated on
the wire. Reaching ~0% needs gapless capture (multi-block or cyclic DMA), which
is a datapath change, not a parameter change.

An earlier 21% figure was a measurement artefact of a short 32-packet capture.
