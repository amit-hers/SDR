#!/usr/bin/env bash
# A config key that is ABSENT takes its default. A key that is PRESENT but
# unparseable must be an error.
#
# These used to fall back to the default on any parse failure, which is the
# worst of both outcomes: the daemon starts, reports nothing, and runs on a
# setting nobody chose. Worse, std::stoi and std::stod accept a PARTIAL parse --
# stoi("1O") is 1, stod("10 dB") is 10 -- so a typo did not even reach the
# fallback; it silently became a different valid value.
set -uo pipefail
DAEMON="${1:?usage: config_strict_test.sh <daemon> <config.json>}"
SRC="${2:?}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# Output is captured before matching, not piped into grep. With `set -o
# pipefail` a pipeline takes the DAEMON's exit status, and the daemon exits
# non-zero on any fatal error -- including the config error being tested for --
# so a correct rejection read as a test failure.
out=$("$DAEMON" --config "$SRC" 2>&1 || true)
case "$out" in
  *"config: "*) echo "FAIL: the checked-in config no longer parses"; exit 1 ;;
esac
echo "  checked-in config still parses"

fails=0
try_bad() {
  local key="$1" val="$2"
  python3 - "$SRC" "$TMP/bad.json" "$key" "$val" <<'PY'
import json, re, sys
src, dst, key, val = sys.argv[1:5]
s = json.dumps(json.load(open(src)), indent=2)
s = re.sub(r'"' + re.escape(key) + r'":\s*[^,\n]+', '"%s": "%s"' % (key, val), s, count=1)
open(dst, 'w').write(s)
PY
  local out
  out=$("$DAEMON" --config "$TMP/bad.json" 2>&1 || true)
  case "$out" in
    *"config: '$key'"*) ;;
    *) echo "  FAIL: '$key': \"$val\" was accepted instead of reported"
       fails=$((fails+1)) ;;
  esac
}

# Partial parses are the dangerous ones: each of these used to succeed quietly.
try_bad bw_mhz          "5x"
try_bad bw_mhz          "1O"
try_bad tx_atten_db     "10 dB"
try_bad fec             "yes"
try_bad rx_queue_depth  "abc"

[ "$fails" -eq 0 ] || { echo "FAIL: $fails unparseable values were accepted"; exit 1; }
echo "  5 unparseable values each reported, none silently defaulted"
echo "PASS"
