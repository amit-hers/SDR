#!/usr/bin/env bash
# Safely bring a conducted (coax) link up to the demodulator's working level.
#
# RUN FROM THE HOST, before the first two-board coax test, with both units
# reachable on their management interfaces.
#
# WHY IT MATTERS: the appliance leaves the transmitter at 0 dB attenuation --
# full output -- because tx_fabric.sh sets it that way. Coupling two boards by
# coax at full power puts the whole transmit output into a receive input whose
# only protection is its own front end. This steps UP from minimum power and
# stops as soon as the far-end demodulator has enough signal.
#
# Deliberately closed-loop: it never relies on an absolute power figure, only on
# the boards' own measurements, stopping at a level KNOWN to work. It therefore
# cannot overshoot because a datasheet number was misread.
#
# MEASURED REFERENCE POINTS (docs/reports/phase9a-loopback-baseline.md):
#   known-good demodulator input : rms 4465, peak 7648  (ADC format 0x51)
#   single-board RF loopback     : rms  395 at RSSI 110.5 dB, RX gain 73 dB,
#                                  i.e. 21.1 dB short -- which is why loopback
#                                  cannot serve as a test bed.
set -uo pipefail

TX_HOST=${1:-192.168.2.17}
RX_HOST=${2:-192.168.2.1}
TARGET=${3:-4465}
PROBE=0x43C30000
P=/sys/bus/iio/devices/iio:device0
SSH=(sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10)

on() { local h=$1; shift; timeout 90 "${SSH[@]}" "root@$h" "$*" </dev/null 2>/dev/null; }

# The far-end demodulator input, as an rms over the IQ probe. The receive chain
# must be MOVING while the probe runs -- the probe records only on tvalid&tready,
# so an undrained chain stalls it and every word reads zero, which looks exactly
# like "no signal" and is not.
far_end_rms() {
    on "$RX_HOST" "
        pkill -x iio_readdev 2>/dev/null
        setsid sh -c 'iio_readdev -b 32768 cf-ad9361-lpc voltage0 voltage1 >/dev/null 2>&1' </dev/null &
        sleep 2
        sh /mnt/jffs2/tools/iq_probe_read.sh $PROBE 128 2>/dev/null
        pkill -x iio_readdev 2>/dev/null
    " | python3 -c '
import sys, math
s = n = 0
for line in sys.stdin:
    line = line.strip()
    if not line.startswith("0x"): continue
    w = int(line, 16)
    i, q = w & 0xFFFF, (w >> 16) & 0xFFFF
    if i >= 32768: i -= 65536
    if q >= 32768: q -= 65536
    s += i*i + q*q; n += 2
print(int(math.sqrt(s/n)) if n else 0)'
}

echo "TX unit $TX_HOST -> RX unit $RX_HOST, target far-end demod rms $TARGET"
echo "stepping transmit attenuation UP from the hardware minimum"
printf '%-12s %-14s %s\n' "tx_atten" "tx_rssi" "far-end demod rms"

att=-89.75
while :; do
    on "$TX_HOST" "echo $att > $P/out_voltage0_hardwaregain"
    sleep 1
    rssi=$(on "$TX_HOST" "cat $P/in_voltage0_rssi")
    rms=$(far_end_rms)
    printf '%-12s %-14s %s\n' "$att dB" "${rssi:-?}" "${rms:-?}"
    if [[ -n "${rms:-}" && "$rms" =~ ^[0-9]+$ ]] && (( rms >= TARGET )); then
        echo
        echo "TARGET REACHED at $att dB attenuation -- stop here."
        echo "Record this figure: it is the working point for this cable and pad,"
        echo "and the starting point for the Phase 11 BER/PER-vs-level curves."
        exit 0
    fi
    att=$(awk -v a="$att" 'BEGIN{printf "%.2f", a+5}')
    if awk -v a="$att" 'BEGIN{exit !(a > 0)}'; then
        echo
        echo "Reached 0 dB attenuation without meeting the target."
        echo "Path loss is too high for this setup. Check the cable, the pad"
        echo "value, and that both units share frequency and sample rate."
        echo "Do NOT conclude the modem is faulty from this alone -- the same"
        echo "symptom came from a stale dd holding the IIO device."
        exit 1
    fi
done
