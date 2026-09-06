#!/usr/bin/env bash
# rollback.sh -- restore a device to the golden (known-good) release.
#   ./scripts/rollback.sh <device> [golden-bundle-dir]
#
# Defaults to the golden bundle recorded alongside this one. The golden release
# exists precisely for the case where a normal release has left a board in a
# state you cannot debug, so this deliberately does the simplest possible thing:
# re-run that bundle's own flash.sh. No special path, no partial restore.
set -uo pipefail
DEV_ARG="${1:-}"
GOLD="${2:-}"
[[ -n "$DEV_ARG" ]] || { echo "usage: $0 <device> [golden-bundle-dir]" >&2; exit 2; }

HERE="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "$GOLD" ]]; then
  for c in "$HERE/../golden" "$HERE/../pluto-datalink-golden" "$HOME/pluto-releases/golden"; do
    [[ -f "$c/manifest.json" ]] && { GOLD="$c"; break; }
  done
fi
if [[ -z "$GOLD" || ! -f "$GOLD/manifest.json" ]]; then
  echo "ERROR: no golden release found." >&2
  echo "       Pass one explicitly:  $0 $DEV_ARG /path/to/pluto-datalink-vX.Y.Z" >&2
  echo "       Golden releases are created with release/promote-golden.sh." >&2
  exit 1
fi
REL=$(python3 -c "import json;print(json.load(open('$GOLD/manifest.json'))['release'])")
TYPE=$(python3 -c "import json;print(json.load(open('$GOLD/manifest.json'))['build_type'])")
if [[ "$TYPE" != "golden" ]]; then
  echo "WARNING: $GOLD is build_type '$TYPE', not 'golden'." >&2
fi
echo "Rolling $DEV_ARG back to $REL ($TYPE) from $GOLD"
exec "$GOLD/scripts/flash.sh" "$DEV_ARG" "$GOLD" --persist
