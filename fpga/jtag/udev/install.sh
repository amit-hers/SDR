#!/usr/bin/env bash
# Install (or remove) the hotplug 6.12 boot.
#
#   sudo fpga/jtag/udev/install.sh install <scratch_dir>
#   sudo fpga/jtag/udev/install.sh remove
#
# Installing does NOT arm it -- a udev rule that reprograms a radio the moment
# it is plugged in should never be on by default. Arm and disarm separately:
#   sudo touch /etc/pluto-autoboot612.enabled     # on
#   sudo rm    /etc/pluto-autoboot612.enabled     # off
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
ACTION="${1:-install}"
[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }

RULE=/etc/udev/rules.d/99-pluto-autoboot612.rules
UNIT=/etc/systemd/system/pluto-autoboot612.service
SCRIPT=/usr/local/sbin/pluto-autoboot612.sh
CONF=/etc/pluto-autoboot612.conf

if [[ "$ACTION" == "remove" ]]; then
  rm -f "$RULE" "$UNIT" "$SCRIPT" "$CONF" /etc/pluto-autoboot612.enabled
  udevadm control --reload-rules 2>/dev/null || true
  systemctl daemon-reload 2>/dev/null || true
  echo "removed"
  exit 0
fi

SCRATCH="${2:-${SDR_SCRATCH:-}}"
[[ -n "$SCRATCH" ]] || { echo "usage: install.sh install <scratch_dir>" >&2; exit 1; }
for f in "$SCRATCH/kbuild612/arch/arm/boot/zImage" \
         "$SCRATCH/rd612/rd-nostdcpp.cpio.gz" \
         "$SCRATCH/boot.dtb"; do
  [[ -s "$f" ]] || { echo "missing build artefact: $f" >&2; exit 1; }
done

install -m 0755 "$HERE/autoboot612.sh"               "$SCRIPT"
install -m 0644 "$HERE/99-pluto-autoboot612.rules"   "$RULE"
install -m 0644 "$HERE/pluto-autoboot612.service"    "$UNIT"
printf 'SDR_ROOT=%s\nSDR_SCRATCH=%s\n' "$ROOT" "$SCRATCH" > "$CONF"
chmod 0644 "$CONF"
udevadm control --reload-rules 2>/dev/null || true
systemctl daemon-reload

echo "installed:"
printf '  %-8s %s\n' rule "$RULE" unit "$UNIT" script "$SCRIPT" conf "$CONF"
echo "  SDR_SCRATCH=$SCRATCH"
echo
echo "currently DISARMED. to arm:  sudo touch /etc/pluto-autoboot612.enabled"
echo "log: /var/log/pluto-autoboot612.log"
