#!/usr/bin/env bash
# sdrctl edits the operator's real config file, so a defect here corrupts data
# rather than merely reporting something wrong. These are the properties that
# matter, each of which has actually been violated:
#
#   * setting every key to its CURRENT value must leave the file byte-identical
#     and still valid JSON. `node_id` is the string "0x00000001", and std::stod
#     parses that quite happily as a hex float -- so it was written back BARE,
#     producing `"node_id": 0x00000001`, which no JSON parser accepts. Editing
#     one field destroyed the whole file.
#   * a field's TYPE comes from the file, not from the text typed. A string key
#     stays quoted even when the value looks numeric.
#   * an unquoted field rejects anything JSON could not parse.
#   * a multi-key set is all-or-nothing.
set -uo pipefail
SDRCTL="${1:?usage: sdrctl_config_test.sh <path-to-sdrctl> <path-to-config.json>}"
SRC="${2:?}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
CFG="$TMP/config.json"
cp "$SRC" "$CFG"
fail() { echo "FAIL: $*"; exit 1; }

python3 - "$SDRCTL" "$CFG" <<'PY' || exit 1
import json, subprocess, sys
S, cfg_path = sys.argv[1], sys.argv[2]
before = open(cfg_path).read()
cfg = json.load(open(cfg_path))
for k, v in cfg.items():
    if isinstance(v, (dict, list)): continue
    cur = subprocess.run([S,'get',k,'--config',cfg_path],
                         capture_output=True, text=True).stdout.strip()
    if not cur: continue
    r = subprocess.run([S,'set',k,cur,'--config',cfg_path], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL: setting {k} to its own value was rejected: {(r.stdout+r.stderr).strip()}")
        sys.exit(1)
    try:
        json.load(open(cfg_path))
    except Exception as e:
        print(f"FAIL: setting {k}={cur} produced invalid JSON: {e}")
        sys.exit(1)
if open(cfg_path).read() != before:
    print("FAIL: round-tripping every key changed the file")
    sys.exit(1)
print(f"  round-tripped {len(cfg)} keys, file byte-identical, still valid JSON")
PY

# A string field keeps its quotes even when the value looks like a number.
if grep -q '"node_id"' "$CFG"; then
  "$SDRCTL" set node_id 0xDEADBEEF --config "$CFG" >/dev/null \
    || fail "could not set node_id"
  grep -q '"node_id": *"0xDEADBEEF"' "$CFG" \
    || fail "node_id lost its quotes -- type was inferred from the value"
  python3 -c "import json,sys; json.load(open('$CFG'))" || fail "config is no longer valid JSON"
  echo "  string field stayed quoted for a numeric-looking value"
fi

# An unquoted field must refuse anything JSON cannot parse.
NUMKEY=$(python3 -c "
import json;c=json.load(open('$CFG'))
print(next((k for k,v in c.items() if isinstance(v,int) and not isinstance(v,bool)),''))")
if [ -n "$NUMKEY" ]; then
  for badval in abc 0x10; do
    if "$SDRCTL" set "$NUMKEY" "$badval" --config "$CFG" >/dev/null 2>&1; then
      fail "unquoted field $NUMKEY accepted '$badval'"
    fi
  done
  python3 -c "import json,sys; json.load(open('$CFG'))" || fail "config damaged by a rejected set"
  echo "  unquoted field rejected non-JSON values, file undamaged"
fi

# All-or-nothing: a bad value in a batch must apply none of it.
if grep -q '"bw_mhz"' "$CFG"; then
  B4=$("$SDRCTL" get bw_mhz --config "$CFG")
  "$SDRCTL" set bw_mhz 1 modulation NOTAMOD --config "$CFG" >/dev/null 2>&1
  [ "$("$SDRCTL" get bw_mhz --config "$CFG")" = "$B4" ] \
    || fail "a rejected batch applied part of its changes"
  echo "  multi-key set was all-or-nothing"
fi
echo "PASS"
