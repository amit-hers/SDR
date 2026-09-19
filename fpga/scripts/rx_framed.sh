#!/bin/sh
# Continuous framed reception at 434 MHz. Run after every PL reload.
#   usage: rx_framed.sh [sample_rate_Hz] [diff_mode] [rx_lo_Hz]
FS=${1:-7680000}
# diff_mode must MATCH the transmitter. tx_fabric.sh takes it as an argument, so
# hardcoding 1 here meant `tx_fabric.sh <fs> <lo> 0` silently produced
# mod=0 / dem=1 -- a combination the demodulator cannot decode, while every
# register still reads healthy and the signal level looks fine. Observed in
# practice: strong signal, balanced symbol statistics, and not one frame.
DIFF=${2:-1}
RXLO=${3:-434000000}
# A MISSING iio_lookup.sh is not a loud failure on busybox: `.` prints "can't
# open" and CARRIES ON, leaving the device paths empty. `dd if= of=cap.bin`
# then reads STDIN and blocks forever, which reads as a dead receiver -- it
# cost a 200 s silent hang. /root is on the ramdisk, so the file really can be
# absent after a reboot. Check before sourcing, and check what it produced.
_D="$(dirname "$0")"
[ -f "$_D/iio_lookup.sh" ] || { echo "ERROR: $_D/iio_lookup.sh not found; restore from /mnt/jffs2/tools" >&2; exit 1; }
. "$_D/iio_lookup.sh"
[ -n "${IIO_TX_DEV:-}" ] && [ -n "${IIO_RX_DEV:-}" ] || { echo "ERROR: IIO devices unresolved" >&2; exit 1; }
P=$IIO_PHY; R=$IIO_RX; D=0x43C00000
echo 0 > $R/buffer/enable 2>/dev/null
devmem 0x79020040 32 0x3            # ADC core out of reset: it gates all of l_clk
devmem 0x7C400080 32 0              # DMA IRQ unmask -- re-masked by every PL reload
echo "$FS" > $P/in_voltage_sampling_frequency
echo 4000000 > $P/in_voltage_rf_bandwidth
# RX LO is a parameter, not a constant.
#
# Hardcoding it forced TX and RX onto one frequency, and the bridge transmits
# idle fill continuously to hold the demodulator's timing lock, so both radios
# keyed up permanently on the same channel and each jammed its own receiver.
# Measured: a unit alone on the air decoded 207,991 false frames with 101,622
# CRC failures and delivered nothing -- it was demodulating itself. Frequency
# division needs A to listen where B transmits, which this could not express.
echo "$RXLO" > $P/out_altvoltage0_RX_LO_frequency
echo slow_attack > $P/in_voltage0_gain_control_mode
# MODEM CORES FIRST, ADC CHANNELS SECOND. The other order latches the sticky
# overflow flag on the first sample -- the adapter has a stream sink that is not
# yet draining and an ADC that cannot be told to wait -- and it never clears.
devmem $((D+0x10)) 32 1             # demod enable
devmem $((D+0x28)) 32 "$DIFF"       # diff_mode -- MUST match tx_fabric.sh's 3rd argument

# Soft-reset the demodulator, WITH ITS OUTPUT DRAINED.
#
# Without this the core comes up stalled and stays there: mu_clamped rails
# (0x7C-0xD0 observed), the symbol stream looks statistically healthy -- close
# to 25% per symbol -- and NOT ONE frame decodes. Every register reads correct
# and the signal can be 24 dB above noise, so it presents as an RF or framing
# problem and is neither. After the reset mu_clamped sits at 0x02-0x04,
# lock_count restarts, and the link runs at PER 0.00%.
#
# The drain is not optional. A stalled core ignores soft_reset while its output
# is backpressured, so something must be emptying the RX DMA at the moment the
# reset is pulsed. iio_readdev is used rather than dd because a raw read() on
# /dev/iio:* does not program the DMA on 6.12 -- it blocks forever and holds the
# device single-open, wedging it for everything afterwards.
# iio_readdev wants the device NAME, not the sysfs path and not "iio:deviceN".
# Passing either yields a 0-byte capture with no error.
_rxname=$(cat "$IIO_RX/name" 2>/dev/null)
setsid sh -c "iio_readdev -b 32768 $_rxname voltage0 voltage1 >/dev/null 2>&1" </dev/null &
_drain=$!
sleep 2
devmem $((D+0x20)) 32 1
sleep 1
devmem $((D+0x20)) 32 0
sleep 1
kill -9 "$_drain" 2>/dev/null
for _p in $(ps | grep '[i]io_readdev' | awk '{print $1}'); do kill -9 "$_p" 2>/dev/null; done
sleep 1
echo "# demod reset: lock=$(devmem $((D+0x18)) 32) mu_clamped=$(devmem $((D+0x30)) 32)"
devmem $((D+0x00)) 32 0x81          # ap_start + auto_restart
# 0x51, not 0x71: dfmt_type must be 0 on ADC core 10.03 or the demodulator input
# rails at 12-bit full scale regardless of RF.
for ch in 0 1 2 3; do devmem $((0x79020400 + 64*ch)) 32 0x51; done
echo 1 > $R/scan_elements/in_voltage0_en 2>/dev/null
echo 1 > $R/scan_elements/in_voltage1_en 2>/dev/null
# At least PKT_BYTES (32768) worth, or the DMA transfer is truncated before the
# packetizer's TLAST arrives. Scan size is 4 bytes, so 16384 here is 65536 bytes
# -- one packet of headroom.
echo 16384 > $R/buffer/length
# Do NOT enable the buffer here. The reader must be iio_readdev, which
# allocates and opens the buffer itself and fails with
#   Unable to allocate buffer: Device or resource busy (16)
# if it is already enabled. A raw read from /dev/iio:deviceN does not work on
# this kernel at all -- it never programs the DMA and blocks forever -- so
# there is no remaining caller that needs the buffer pre-enabled. Measured from
# a clean boot: iio_readdev returned 524288 B in 4 s with 17 DMA interrupts,
# against a raw read that never returned.
echo 0 > $R/buffer/enable 2>/dev/null || true
sleep 1
echo "# rx_lo=$(cat $P/out_altvoltage0_RX_LO_frequency) fs=$(cat $P/in_voltage_sampling_frequency) l_clk=$(devmem 0x79020054 32) rssi=$(cat $P/in_voltage0_rssi) gain=$(cat $P/in_voltage0_hardwaregain) diff=$(devmem $((D+0x28)) 32) ap=$(devmem $((D+0x00)) 32) up=$(cut -d. -f1 /proc/uptime)s"
