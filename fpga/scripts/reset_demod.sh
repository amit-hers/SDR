#!/bin/sh
# Pulse the demodulator's soft-reset register on a LIVE appliance -- one
# where sdr_bridge is already running and already holds the RX IIO device
# open, its own internal iio_readdev loop already draining the RX DMA
# fabric. That drain is the soft-reset core's precondition: pulse the
# register while nothing is reading and the core just ignores it and stalls.
#
# rx_framed.sh's bring-up reset dance satisfies that same precondition by
# spawning a SEPARATE `iio_readdev` process, because at bring-up nothing else
# is reading yet. Reusing that dance here would try to open a SECOND reader
# on a single-open IIO character device the bridge already holds open --
# EBUSY, not a reset. So this script does only the pulse: no drain process,
# no wait for the bridge to release anything, because it never needs to.
#
# Deliberately not a general register-poke tool: the address and pulse shape
# are fixed and never taken from an argument, so sdr-agent's
# /api/v1/control/reset_demod cannot be turned into an arbitrary devmem
# primitive by anything a caller sends.
set -eu
D=0x43C00000
devmem $((D+0x20)) 32 1
sleep 1
devmem $((D+0x20)) 32 0
sleep 1
echo "demod reset pulsed: lock=$(devmem $((D+0x18)) 32) mu_clamped=$(devmem $((D+0x30)) 32)"
