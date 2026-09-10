#!/bin/sh
# Bring the radio up and start the IP bridge. Run ON the Pluto.
#
#   usage: bridge_up.sh <local_ip> <peer_ip> [sample_rate] [lo_hz] [extra args...]
#   e.g.:  bridge_up.sh 172.30.99.1 172.30.99.2 3840000 434000000 --forward
#
# The bridge moves bytes; it deliberately does NOT duplicate the bring-up,
# because the bring-up order is load-bearing and is already established here and
# in tx_fabric.sh / rx_framed.sh. Getting it wrong does not produce an error --
# it produces a radio that looks healthy in every register and carries nothing:
#
#  * The modem cores must start BEFORE the ADC channels are enabled, or the
#    sticky overflow latch fires on the first sample and never clears.
#  * Opening the transmit buffer re-points the DAC at the internal DDS, so the
#    DMA source must be selected AFTER that, never before.
#  * The DAC datarate register defaults to 0, which plays samples out at twice
#    the intended rate and halves samples-per-symbol.
set -u
LOCAL=${1:?usage: bridge_up.sh <local_ip> <peer_ip> [rate] [lo] [args...]}
PEER=${2:?usage: bridge_up.sh <local_ip> <peer_ip> [rate] [lo] [args...]}
FS=${3:-3840000}
LO=${4:-434000000}
shift 4 2>/dev/null || shift $#
DIR="$(dirname "$0")"
. "$DIR/iio_lookup.sh"
BRIDGE=${BRIDGE:-/tmp/sdr_bridge}

[ -x "$BRIDGE" ] || { echo "bridge_up: $BRIDGE not found or not executable"; exit 1; }

# A holder left by an earlier run makes every open fail with EBUSY, which is
# the failure that most resembles a dead radio. Clear it first, and heed the
# unrecoverable verdict rather than pressing on into an EBUSY.
sh "$DIR/watchdog_relax.sh" 120 >/dev/null 2>&1
sh "$DIR/free_capture_dev.sh" || {
    echo "bridge_up: the char devices could not be freed; not starting."
    exit 2
}

sh "$DIR/tx_fabric.sh" "$FS" "$LO" 1 || exit 1
sh "$DIR/rx_framed.sh" "$FS"        || exit 1

# Transmit buffer, then DMA source -- in that order, per the note above.
DB=0x79024000
echo 1 > "$IIO_TX/scan_elements/out_voltage0_en" 2>/dev/null
echo 1 > "$IIO_TX/scan_elements/out_voltage1_en" 2>/dev/null
echo 32768 > "$IIO_TX/buffer/length"
echo 1 > "$IIO_TX/buffer/enable"
for ch in 0 1; do
  devmem $((DB+0x418+64*ch)) 32 2     # channel source = DMA
  devmem $((DB+0x400+64*ch)) 32 1     # channel enable
done

echo "bridge_up: fs=$FS lo=$LO tx=$IIO_TX_DEV rx=$IIO_RX_DEV"
exec "$BRIDGE" --local "$LOCAL" --peer "$PEER" \
     --tx "$IIO_TX_DEV" --rx "$IIO_RX_DEV" "$@"
