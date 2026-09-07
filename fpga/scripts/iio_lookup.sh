# iio_lookup.sh -- resolve IIO devices BY NAME. Source this; do not run it.
#
# Device INDICES ARE NOT STABLE. They depend on which drivers probed, and that
# changes with the rootfs: flashing the stock firmware removed the pga102 device
# and shifted everything down by one, so the TX device moved from iio:device3 to
# iio:device2 and the RX device from iio:device4 to iio:device3. A script that
# hard-codes the old numbers then writes its transmit stream into the RECEIVE
# device, which fails with "Operation not permitted" -- an error that says
# nothing about the actual cause and cost a full debugging cycle to trace.
#
#   IIO_PHY  ad9361-phy              control: LO, sample rate, gain, ensm
#   IIO_TX   cf-ad9361-dds-core-lpc  DAC buffer; on the modem PL this takes BYTES
#   IIO_RX   cf-ad9361-lpc           ADC buffer; on the modem PL this gives BYTES
iio_by_name() {
    for d in /sys/bus/iio/devices/iio:device*; do
        [ "$(cat "$d/name" 2>/dev/null)" = "$1" ] && { echo "$d"; return 0; }
    done
    echo "ERROR: no IIO device named '$1'" >&2
    return 1
}
iio_chardev() { echo "/dev/$(basename "$1")"; }

IIO_PHY=$(iio_by_name ad9361-phy)             || exit 1
IIO_TX=$(iio_by_name cf-ad9361-dds-core-lpc)  || exit 1
IIO_RX=$(iio_by_name cf-ad9361-lpc)           || exit 1
IIO_TX_DEV=$(iio_chardev "$IIO_TX")
IIO_RX_DEV=$(iio_chardev "$IIO_RX")
