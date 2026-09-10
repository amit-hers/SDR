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
. "$(dirname "$0")/iio_lookup.sh"
DB=0x79024000; D=$IIO_TX
echo 0 > $D/buffer/enable 2>/dev/null || true
echo 1 > $D/scan_elements/out_voltage0_en 2>/dev/null || true
echo 1 > $D/scan_elements/out_voltage1_en 2>/dev/null || true
echo "$NB" > $D/buffer/length
echo 1 > $D/buffer/enable
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
# The char device is SINGLE-OPEN. A holder left by an earlier run makes the
# write fail with EBUSY -- and a feed that never happened looks exactly like a
# transmitter that emits nothing: zero IQ at the probe, zero symbols at the far
# end, every register still reading healthy. Check BEFORE feeding and name the
# cause, rather than leaving it to be inferred from a silent radio.
holder=$(ps w 2>/dev/null | grep '[i]io:device' | grep -v "$$" | head -1)
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
if ! cat "$F" > "$IIO_TX_DEV"; then
  echo "# ERROR: write to $IIO_TX_DEV failed -- NOTHING was transmitted"
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
