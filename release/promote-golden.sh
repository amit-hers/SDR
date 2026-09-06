#!/usr/bin/env bash
# promote-golden.sh -- mark one validated release as the known-good recovery image.
#   ./release/promote-golden.sh v1.3.0
#
# Promotion is EXPLICIT and never automatic. The golden release is the thing you
# reach for when a board is in a state you cannot debug, so it must not be
# quietly replaced by whatever shipped most recently -- the newest release is
# the least proven one, and that is exactly backwards for a recovery image.
#
# Requires the release to already exist as a built artifact from an annotated
# tag, and to have passed hardware validation (recorded by --validated).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-}"
[[ -n "$VERSION" ]] || { echo "usage: $0 <version>   e.g. $0 v1.3.0" >&2; exit 2; }

OUT="$ROOT/out"
NAME="pluto-datalink-${VERSION}"
BUNDLE="$OUT/$NAME"
SECONDARY="${SDR_GOLDEN_MIRROR:-$HOME/pluto-releases}"

[[ -d "$BUNDLE" ]] || { echo "ERROR: $BUNDLE does not exist. Build it first." >&2; exit 1; }
[[ -f "$BUNDLE/manifest.json" ]] || { echo "ERROR: $BUNDLE has no manifest.json" >&2; exit 1; }

TYPE=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['build_type'])")
TAG=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['git_tag'])")
DIRTY=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['tree_dirty'])")

if [[ "$TYPE" != "release" && "$TYPE" != "golden" ]]; then
  echo "ERROR: $VERSION is build_type '$TYPE'." >&2
  echo "       Only a hardware-validated 'release' may be promoted to golden." >&2
  exit 1
fi
if [[ -z "$TAG" ]]; then
  echo "ERROR: $VERSION was not built from an annotated git tag." >&2
  echo "       A golden release must be rebuildable by someone else, from an immutable ref." >&2
  exit 1
fi
if [[ "$DIRTY" == "True" ]]; then
  echo "ERROR: $VERSION was built from a dirty tree and cannot be reproduced." >&2
  exit 1
fi
if [[ ! -f "$BUNDLE/.validated" ]]; then
  echo "ERROR: $VERSION has no hardware-validation record." >&2
  echo "       Run:  ./release/mark-validated.sh $VERSION \"<what was tested>\"" >&2
  exit 1
fi

# Re-verify before promoting: an artifact that has sat on disk is exactly the
# one worth re-checking, and a golden image that fails its own checksums is
# worse than none.
( cd "$BUNDLE" && sha256sum -c --quiet checksums.sha256 ) \
  || { echo "ERROR: checksum verification failed for $BUNDLE" >&2; exit 1; }

python3 - "$BUNDLE" <<PY
import json,sys,os
p=os.path.join(sys.argv[1],"manifest.json")
m=json.load(open(p)); m["build_type"]="golden"; m["retention"]="permanent"
m["promoted_utc"]=__import__("datetime").datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
json.dump(m,open(p,"w"),indent=2)
PY
( cd "$BUNDLE" && find . -type f ! -name checksums.sha256 -print0 | sort -z | xargs -0 sha256sum > checksums.sha256 )
echo "golden" > "$OUT/$NAME.class"
( cd "$OUT" && rm -f "$NAME.tar.zst" && tar --zstd -cf "$NAME.tar.zst" "$NAME" \
   && sha256sum "$NAME.tar.zst" > "$NAME.sha256" )

ln -sfn "$BUNDLE" "$OUT/golden"
echo "$VERSION" > "$OUT/GOLDEN"

# Second, independent location. A recovery image on one disk is not a backup.
mkdir -p "$SECONDARY/golden"
cp -f "$OUT/$NAME.tar.zst" "$OUT/$NAME.sha256" "$SECONDARY/golden/"
rm -rf "$SECONDARY/golden/$NAME" && cp -a "$BUNDLE" "$SECONDARY/golden/$NAME"
( cd "$SECONDARY/golden" && sha256sum -c --quiet "$NAME.sha256" ) \
  || { echo "ERROR: the mirrored copy does not verify." >&2; exit 1; }

echo "Promoted $VERSION to GOLDEN"
echo "  primary:   $BUNDLE"
echo "  secondary: $SECONDARY/golden/$NAME"
echo "  rollback:  $BUNDLE/scripts/rollback.sh UNIT-A $BUNDLE"
