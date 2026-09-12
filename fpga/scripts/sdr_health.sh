#!/bin/sh
# One-shot health report. Run ON the Pluto. Answers the question an operator
# actually has: is the problem RF, CPU, FPGA, network, or configuration?
#
# Every check here exists because its absence cost real debugging time. The
# order is deliberate -- each section can invalidate the ones below it, so the
# first FAIL is usually the whole story:
#
#   identity  a wrong or absent bitstream makes every register read meaningless
#   drivers   maia-sdr holds the IIO buffers; nothing transfers while it is loaded
#   buffers   a stale holder returns EBUSY and reads exactly like a dead radio
#   dma       transfers that never start look identical to a silent transmitter
#   rf        the config that decides whether anything can be heard at all
#   level     the ONLY trustworthy signal measurement on this board
#
# Exit status: 0 all good, 1 one or more FAIL.
set -u
D="$(dirname "$0")"
FAIL=0
ok()   { printf "  [ ok ] %s\n" "$*"; }
bad()  { printf "  [FAIL] %s\n" "$*"; FAIL=1; }
note() { printf "  [note] %s\n" "$*"; }

echo "=== IDENTITY ==="
echo "  uptime      : $(cut -d. -f1 /proc/uptime)s   kernel $(uname -r)"
echo "  release     : $(cat /mnt/jffs2/sdr-release 2>/dev/null || echo '(none)')"
echo "  serial      : $(cat /mnt/jffs2/serial.txt 2>/dev/null || echo '(none)')"
MAGIC=$(devmem 0x43C50000 32 2>/dev/null)
case "$MAGIC" in
  0x5344524C) ok "FPGA identity 0x5344524C, ABI $(devmem 0x43C50008 32 2>/dev/null), regmap $(devmem 0x43C5000C 32 2>/dev/null)" ;;
  "")         bad "0x43C50000 bus-errors: the MODEM bitstream is not loaded (stock PL?). fpga_manager says '$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)', which it reports for the stock image too." ;;
  *)          bad "FPGA magic is $MAGIC, expected 0x5344524C" ;;
esac

echo "=== DRIVERS AND SERVICES ==="
if [ "$(lsmod 2>/dev/null | grep -c maia)" -gt 0 ]; then
  bad "maia_sdr is loaded -- it holds the IIO buffers and NOTHING will transfer. Flash a release built after the ramdisk strip."
else
  ok "maia_sdr absent"
fi
WD=$(ps w 2>/dev/null | grep '[w]atchdog -t' | head -1)
case "$WD" in
  *"-T 10"*) bad "watchdog is the stock -T 10: a devmem burst or a large transfer will reset the board mid-test" ;;
  *"-T "*)   ok "watchdog relaxed ($(echo "$WD" | grep -o '\-T [0-9]*'))" ;;
  *)         note "no userspace watchdog running" ;;
esac
echo "  tools       : $(ls /root/sdr-tools/*.sh 2>/dev/null | wc -l) in /root, $(ls /mnt/jffs2/tools/*.sh 2>/dev/null | wc -l) in jffs2 (jffs2 is the one that survives a reboot)"

echo "=== IIO AND BUFFERS ==="
if [ -f "$D/iio_lookup.sh" ]; then
  . "$D/iio_lookup.sh"
  ok "TX=$IIO_TX_DEV RX=$IIO_RX_DEV (resolved by NAME; indices move between rootfs versions)"
  # Match /dev/iio:deviceN specifically. Matching "iio:device" anywhere also
  # catches processes watching SYSFS attributes under
  # /sys/bus/iio/devices/iio:deviceN/, which hold nothing -- the firmware runs
  # inotifywait on ensm_mode and RX_LO_frequency, and reporting those as
  # holders sends an operator after a problem that does not exist.
  HOLD=$(ps w 2>/dev/null | grep "/dev/iio:device" | grep -v "[g]rep" | head -1)
  [ -n "$HOLD" ] && bad "a process holds an IIO char device: $HOLD" || ok "no stale holder"
  for p in /proc/[0-9]*; do
    st=$(awk '/^State:/{print $2}' "$p/status" 2>/dev/null)
    [ "$st" = "D" ] && case "$(cat "$p/wchan" 2>/dev/null)" in
      *iio*|*dma_buffer*|*enable_store*) bad "pid ${p#/proc/} is wedged in $(cat "$p/wchan"); only a POWER CYCLE clears this" ;;
    esac
  done
else
  bad "iio_lookup.sh missing beside this script; restore from /mnt/jffs2/tools"
fi

echo "=== DMA ==="
IRQ=$(grep -E "7c4[02]0000.dma" /proc/interrupts 2>/dev/null | awk '{s+=$2+$3} END {print s+0}')
echo "  interrupts  : $IRQ"
echo "  tx bytes    : $(cat /sys/class/dma/dma1chan0/bytes_transferred 2>/dev/null || echo n/a)"
echo "  tx dmac     : ctrl=$(devmem 0x7C420400 32 2>/dev/null) xlen=$(devmem 0x7C420418 32 2>/dev/null)"
[ "${IRQ:-0}" = "0" ] && note "no DMA interrupt has fired since boot. Normal if nothing has transferred yet; if a transfer IS running, the transfers are not starting -- and note a raw read/write on /dev/iio:deviceN never programs the DMA on this kernel. Use iio_readdev / iio_writedev."

echo "=== RF CONFIGURATION ==="
if [ -n "${IIO_PHY:-}" ]; then
  P=$IIO_PHY
  echo "  TX LO       : $(cat $P/out_altvoltage1_TX_LO_frequency 2>/dev/null)   atten $(cat $P/out_voltage0_hardwaregain 2>/dev/null)"
  echo "  RX LO       : $(cat $P/out_altvoltage0_RX_LO_frequency 2>/dev/null)   gain  $(cat $P/in_voltage0_hardwaregain 2>/dev/null) ($(cat $P/in_voltage0_gain_control_mode 2>/dev/null))"
  echo "  ports       : tx=$(cat $P/out_voltage0_rf_port_select 2>/dev/null) rx=$(cat $P/in_voltage0_rf_port_select 2>/dev/null)   ensm=$(cat $P/ensm_mode 2>/dev/null)"
  echo "  rates       : tx=$(cat $P/out_voltage_sampling_frequency 2>/dev/null) rx=$(cat $P/in_voltage_sampling_frequency 2>/dev/null)"
  note "in_voltage0_rssi is NOT trustworthy here -- it reads a constant in manual-gain mode. Judge level with the RX IQ probe (iq_probe_read.sh 0x43C30000) instead."
fi

echo "=== TEMPERATURE AND LOAD ==="
# No thermal_zone on this board; the SoC die temperature comes from the XADC,
# as raw/offset/scale in millidegrees.
XADC=$(ls -d /sys/bus/iio/devices/iio:device* 2>/dev/null | while read d; do
         [ "$(cat "$d/name" 2>/dev/null)" = "xadc" ] && echo "$d"; done | head -1)
if [ -n "$XADC" ] && [ -f "$XADC/in_temp0_raw" ]; then
  R=$(cat "$XADC/in_temp0_raw" 2>/dev/null)
  O=$(cat "$XADC/in_temp0_offset" 2>/dev/null || echo 0)
  S=$(cat "$XADC/in_temp0_scale" 2>/dev/null || echo 1)
  echo "  soc die     : $(awk "BEGIN{printf \"%.1f\", ($R + $O) * $S / 1000}") C"
else
  echo "  soc die     : (xadc not available)"
fi
for z in /sys/class/thermal/thermal_zone*/temp; do
  [ -f "$z" ] || continue
  echo "  $(basename "$(dirname "$z")"): $(( $(cat "$z") / 1000 )) C"
done
echo "  load        : $(cut -d' ' -f1-3 /proc/loadavg)"
echo "  memfree     : $(awk '/MemAvailable/{print $2" kB"}' /proc/meminfo)"

echo
[ "$FAIL" = "0" ] && echo "RESULT: healthy" || echo "RESULT: PROBLEMS FOUND (see [FAIL] above)"
exit $FAIL
