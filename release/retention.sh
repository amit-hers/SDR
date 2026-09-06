#!/usr/bin/env bash
# retention.sh -- prune expired artifacts. Never touches release or golden.
#   ./release/retention.sh [--dry-run]
#
#   dev      14 days
#   rc       90 days
#   release  permanent
#   golden   permanent, and mirrored
#
# The retention class is read from the artifact's own .class file, written at
# build time, so pruning does not depend on anyone remembering what a build was
# for. Anything whose class cannot be determined is KEPT, not deleted -- the
# failure mode of a cleanup script must be leaving too much, never too little.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/out"
DRY=0; [[ "${1:-}" == "--dry-run" ]] && DRY=1
[[ -d "$OUT" ]] || { echo "nothing to prune"; exit 0; }

now=$(date -u +%s)
kept=0; removed=0
for cls in "$OUT"/*.class; do
  [[ -e "$cls" ]] || continue
  name=$(basename "$cls" .class)
  type=$(cat "$cls")
  case "$type" in
    release|golden) echo "KEEP   $name ($type, permanent)"; kept=$((kept+1)); continue ;;
    dev)  max=$((14*86400)) ;;
    rc)   max=$((90*86400)) ;;
    *)    echo "KEEP   $name (unknown class '$type' -- not deleting)"; kept=$((kept+1)); continue ;;
  esac
  age=$(( now - $(stat -c%Y "$cls") ))
  if (( age > max )); then
    printf 'PRUNE  %s (%s, %d days old)\n' "$name" "$type" $((age/86400))
    if (( DRY == 0 )); then rm -rf "$OUT/$name" "$OUT/$name.tar.zst" "$OUT/$name.sha256" "$cls"; fi
    removed=$((removed+1))
  else
    printf 'KEEP   %s (%s, %d of %d days)\n' "$name" "$type" $((age/86400)) $((max/86400))
    kept=$((kept+1))
  fi
done
echo
echo "kept $kept, $([[ $DRY -eq 1 ]] && echo "would prune" || echo pruned) $removed"
