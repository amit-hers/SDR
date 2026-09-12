#!/usr/bin/env bash
# A config write must survive a crash or a power cut at any instant.
#
# rename(2) is atomic with respect to the directory entry, but it says nothing
# about whether the FILE's data reached the disk. Without fsync a power loss
# moments after the rename can leave a correctly named, correctly sized,
# ZERO-FILLED config -- the radio then boots with a file that parses as garbage
# rather than with its previous settings, which is worse than either outcome
# the temporary file was meant to prevent. The board keeps this on flash and
# loses power without warning, so it is not a theoretical concern.
#
# These check the observable consequences: the file is always either the old
# content or the new one, never a fragment; and no temporary is left behind on
# either path, because a partition full of .tmp files eventually stops the next
# write from being creatable at all.
set -uo pipefail
SDRCTL="${1:?usage: sdrctl_atomic_test.sh <path-to-sdrctl> <path-to-config.json>}"
SRC="${2:?}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
CFG="$TMP/config.json"
cp "$SRC" "$CFG"
fail() { echo "FAIL: $*"; exit 1; }

# ── 1. a successful write leaves no temporary ────────────────────────────
BEFORE=$(ls -1 "$TMP" | wc -l)
"$SDRCTL" set freq_tx_mhz 435 --config "$CFG" >/dev/null 2>&1 \
  || fail "set returned non-zero"
AFTER=$(ls -1 "$TMP")
[[ "$(echo "$AFTER" | wc -l)" == "$BEFORE" ]] || fail "extra files left behind: $AFTER"
echo "$AFTER" | grep -q '\.tmp' && fail "a .tmp file was left behind: $AFTER"
python3 -c "import json,sys; json.load(open('$CFG'))" 2>/dev/null \
  || fail "config is not valid JSON after a write"
grep -q 435 "$CFG" || fail "the value was not actually written"
echo "  write succeeded, no temporary left, JSON still valid"

# ── 2. a write that CANNOT complete must leave the original intact ───────
# A read-only directory makes both the temporary creation and the rename fail.
cp "$CFG" "$TMP/reference.json"
chmod 500 "$TMP"
"$SDRCTL" set freq_tx_mhz 436 --config "$CFG" >/dev/null 2>&1
RC=$?
chmod 700 "$TMP"
if [[ $RC -eq 0 ]]; then
  # If it somehow succeeded the file must still be coherent.
  python3 -c "import json;json.load(open('$CFG'))" 2>/dev/null || fail "write reported success but left invalid JSON"
  echo "  NOTE: write succeeded despite a read-only directory (running as root?)"
else
  cmp -s "$CFG" "$TMP/reference.json" || fail "a FAILED write modified the original config"
  ls -1 "$TMP" | grep -q '\.tmp' && fail "a failed write left a .tmp file behind"
  echo "  failed write left the original byte-identical and no temporary"
fi

# ── 3. the file is never a fragment: repeated writes always parse ────────
for i in 1 2 3 4 5 6 7 8; do
  "$SDRCTL" set freq_tx_mhz "$(( 430 + i ))" --config "$CFG" >/dev/null 2>&1 || true
  python3 -c "import json,sys; json.load(open('$CFG'))" 2>/dev/null \
    || fail "config became unparseable on write $i"
done
ls -1 "$TMP" | grep -q '\.tmp' && fail "temporaries accumulated over repeated writes"
echo "  8 consecutive writes, always valid JSON, no temporaries accumulated"

echo "PASS"
