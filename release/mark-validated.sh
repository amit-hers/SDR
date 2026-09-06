#!/usr/bin/env bash
# mark-validated.sh -- record that a release passed on real hardware.
#   ./release/mark-validated.sh v1.3.0 "PER 0.00% over 12709 frames at 17.28 MS/s"
#
# Separate from building on purpose. A build says the artifact assembled; this
# says a human put it on a board and watched it work. Only the second one
# justifies permanent retention, and promote-golden.sh refuses without it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-}"; NOTE="${2:-}"
[[ -n "$VERSION" && -n "$NOTE" ]] || { echo "usage: $0 <version> \"<evidence>\"" >&2; exit 2; }
BUNDLE="$ROOT/out/pluto-datalink-${VERSION}"
[[ -d "$BUNDLE" ]] || { echo "ERROR: no such release: $BUNDLE" >&2; exit 1; }
cat > "$BUNDLE/.validated" <<EOF
validated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
validated_by=$(git config user.name 2>/dev/null || whoami)
evidence=$NOTE
EOF
( cd "$BUNDLE" && find . -type f ! -name checksums.sha256 -print0 | sort -z | xargs -0 sha256sum > checksums.sha256 )
echo "Recorded hardware validation for $VERSION:"
sed 's/^/  /' "$BUNDLE/.validated"
