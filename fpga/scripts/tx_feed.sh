#!/bin/sh
# Stream a byte file through the fabric modulator. Run ON the Pluto, after
# tx_fabric.sh.  usage: tx_feed.sh <file> [buffer_samples]
#
# The write to the TX char device blocks until the DAC consumes, so the file is
# paced by the link itself -- no rate limiting is needed or wanted here.
#
# Channel source is selected AFTER the buffer is enabled: opening the buffer
# re-points the DAC at the internal DDS, so an earlier write is silently undone
# and the DAC emits nothing while every register still reads healthy.
set -u
F=${1:-/tmp/vid.bytes}; NB=${2:-32768}
# A MISSING iio_lookup.sh is not a loud failure on busybox: `.` prints "can't
# open" and CARRIES ON, leaving the device paths empty. `dd if= of=cap.bin`
# then reads STDIN and blocks forever, which reads as a dead receiver -- it
# cost a 200 s silent hang. /root is on the ramdisk, so the file really can be
# absent after a reboot. Check before sourcing, and check what it produced.
_D="$(dirname "$0")"
[ -f "$_D/iio_lookup.sh" ] || { echo "ERROR: $_D/iio_lookup.sh not found; restore from /mnt/jffs2/tools" >&2; exit 1; }
. "$_D/iio_lookup.sh"
[ -n "${IIO_TX_DEV:-}" ] && [ -n "${IIO_RX_DEV:-}" ] || { echo "ERROR: IIO devices unresolved" >&2; exit 1; }
DB=0x79024000; D=$IIO_TX
echo 0 > $D/buffer/enable 2>/dev/null || true
echo 1 > $D/scan_elements/out_voltage0_en 2>/dev/null || true
echo 1 > $D/scan_elements/out_voltage1_en 2>/dev/null || true
echo "$NB" > $D/buffer/length
# Do NOT enable the buffer here. iio_writedev allocates and opens it itself,
# and a buffer already enabled makes that fail with
#   Unable to allocate buffer: Device or resource busy (16)
# tx_cyclic.sh does not enable it either, for the same reason.
echo 0 > $D/buffer/enable 2>/dev/null || true
# Both registers, per channel, matching tx_bringup.sh -- the bring-up that was
# validated on the modem PL. 0x418 selects the DMA source and is the one that
# demonstrably matters: tx_cyclic.sh sets only it and the DAC emits. 0x400 is
# the DDS scale, unused once the source is DMA, and is set here only to stay
# identical to the validated sequence rather than because it is known to be
# required. Do not read it as the fix for a silent transmitter.
for ch in 0 1; do
  devmem $((DB+0x418+64*ch)) 32 2    # channel source = DMA
  devmem $((DB+0x400+64*ch)) 32 1    # channel enable
done
# iio_writedev opens the buffer itself, and opening it re-points the channel at
# the internal DDS -- so the source select above is undone and has to be redone
# once the writer is running. tx_cyclic.sh learned this the same way.
IIO_TX_NAME=cf-ad9361-dds-core-lpc
# The char device is SINGLE-OPEN. A holder left by an earlier run makes the
# write fail with EBUSY -- and a feed that never happened looks exactly like a
# transmitter that emits nothing: zero IQ at the probe, zero symbols at the far
# end, every register still reading healthy. Check BEFORE feeding and name the
# cause, rather than leaving it to be inferred from a silent radio.
# Match THIS device only. A concurrent capture on the RX char device is
# legitimate and must not be read as a holder -- a single-board loopback runs
# both directions at once, and a broader match refuses the very test it is
# meant to protect. busybox ps may render the argument without its '=', so key
# on the device path rather than on "of=".
# Exclude the pipeline's own grep processes: their argv contains the device
# path, so a naive match finds itself and reports the device held when it is
# free. Also exclude this script's own pid.
holder=$(ps w 2>/dev/null | grep "$IIO_TX_DEV" | grep -v "[g]rep" | grep -v "^ *$$ " | head -1)
if [ -n "$holder" ]; then
  echo "# ERROR: $IIO_TX_DEV is already held: $holder"
  echo "#        run free_capture_dev.sh first"
  exit 1
fi

NBYTES=$(wc -c < "$F")
T0=$(cut -d. -f1 /proc/uptime)
echo "# feeding $NBYTES B, src=$(devmem $((DB+0x418)) 32), start=${T0}s"
# Do NOT discard this stderr. EBUSY here was invisible for an entire debugging
# session because a caller redirected it away, and every downstream measurement
# was then describing a write that never occurred.
# THROUGH libiio, NOT a raw write to the char device. On the Pluto+ 6.12.77
# kernel a direct write()/cat to /dev/iio:deviceN never programs the DMA at
# all: ctrl, flags and x_length stay at their reset values, no interrupt ever
# fires, bytes_transferred stays 0 and the write blocks forever -- while every
# register reads healthy. Measured on the same board minutes apart, raw write
# gave an all-zero modulator output and libiio gave 64/64 non-zero with
# interrupts firing. This is what made the transmitter look dead.
# Start the writer FIRST, then re-select the DMA source. Opening the buffer
# re-points the channel at the internal DDS, so a source select made before
# this point is silently undone and the DAC emits nothing while every register
# still reads healthy -- the failure tx_cyclic.sh documents.
setsid sh -c "iio_writedev -b $NB $IIO_TX_NAME voltage0 voltage1 < '$F' > /dev/null 2>/tmp/tx_feed.err" < /dev/null &
sleep 3
for ch in 0 1; do devmem $((DB+0x418+64*ch)) 32 2; done
# Wait for it to finish streaming the file.
while [ "$(ps w 2>/dev/null | grep -c '[i]io_writedev')" -gt 0 ]; do sleep 1; done
if [ -s /tmp/tx_feed.err ]; then
  echo "# ERROR: iio_writedev reported: $(head -2 /tmp/tx_feed.err)"
  exit 1
fi
T1=$(cut -d. -f1 /proc/uptime)
EL=$((T1-T0))
echo "# done, end=${T1}s elapsed=${EL}s"
# The write is paced by the DAC, so the elapsed time is the honest check that
# the bytes really went out: 1 byte -> 4 QPSK symbols -> 4*SPS samples. Far too
# fast means the buffer swallowed them without the fabric consuming.
if [ "$EL" -eq 0 ] && [ "$NBYTES" -gt 262144 ]; then
  echo "# WARNING: $NBYTES B drained in under a second. The link cannot be that"
  echo "#          fast -- the bytes did not reach the DAC. Check the buffer is"
  echo "#          enabled and that no stale holder owns the device."
  exit 1
fi
