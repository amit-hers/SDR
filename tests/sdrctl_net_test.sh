#!/usr/bin/env bash
# `sdrctl net set` edits config.txt on the radio's mass-storage volume, which is
# the device's BOOT configuration. Two properties, both of which have been
# violated:
#
#   * a value may not inject additional lines. A hostname of
#     "evil\r\nipaddr = 9.9.9.9" added a whole extra line, and since the board
#     reads the first ipaddr it finds, the radio booted on an address nobody
#     set. A value that can add lines can change the board's identity.
#   * CRLF must survive. The volume is FAT and the board's parser ignores a line
#     whose terminator has been stripped, so a rewrite that "helpfully"
#     normalises line endings silently disables every setting it touched.
set -uo pipefail
SDRCTL="${1:?usage: sdrctl_net_test.sh <path-to-sdrctl>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
CFG="$TMP/config.txt"

fresh() {
  printf '[NETWORK]\r\nhostname = pluto\r\nipaddr = 192.168.2.1\r\nipaddr_host = 192.168.2.10\r\nnetmask = 255.255.255.0\r\n[SYSTEM]\r\nudc_handle_suspend = 0\r\n' > "$CFG"
}
fail() { echo "FAIL: $*"; exit 1; }

# 1. A normal edit changes one field and keeps every CRLF.
fresh
before_lines=$(grep -c $'\r$' "$CFG")
"$SDRCTL" net set --ip 192.168.2.17 --volume "$CFG" >/dev/null 2>&1 \
  || fail "a valid --ip was rejected"
grep -q $'^ipaddr = 192.168.2.17\r$' "$CFG" || fail "--ip did not take, or lost its CRLF"
grep -q $'^ipaddr_host = 192.168.2.10\r$' "$CFG" || fail "ipaddr_host was damaged by an ipaddr edit"
[ "$(grep -c $'\r$' "$CFG")" = "$before_lines" ] || fail "line endings were not preserved"
echo "  edit changed one field, CRLF intact, neighbouring key untouched"

# 2. Injection through a value must be refused.
fresh
"$SDRCTL" net set --hostname "$(printf 'evil\r\nipaddr = 9.9.9.9')" --volume "$CFG" >/dev/null 2>&1
[ "$(grep -c '^ipaddr = ' "$CFG")" = "1" ] || fail "a line break in a value injected a config line"
grep -q '9.9.9.9' "$CFG" && fail "injected address reached the file"
echo "  line break in a value refused, no line injected"

# 3. A line break with NO '=' in it. The payload in (2) contains an '=', so the
# '=' guard alone would catch it and the line-break guard would appear tested
# when it was not -- removing it left this suite green. A section header injects
# just as effectively and carries no '='.
fresh
"$SDRCTL" net set --hostname "$(printf 'evil\r\n[SYSTEM]')" --volume "$CFG" >/dev/null 2>&1
[ "$(grep -c '^\[SYSTEM\]' "$CFG")" = "1" ] || fail "a line break injected a section header"
grep -q '^hostname = evil\r\?$' "$CFG" && fail "the value was truncated and written instead of refused"
echo "  line break without '=' refused"

# 4. An '=' would also make one field look like two.
fresh
"$SDRCTL" net set --hostname 'a = b' --volume "$CFG" >/dev/null 2>&1
grep -q '^hostname = a = b' "$CFG" && fail "'=' in a value was written"
echo "  '=' in a value refused"

# 5. A legitimate hostname still works.
fresh
"$SDRCTL" net set --hostname plutoplus-a --volume "$CFG" >/dev/null 2>&1 \
  || fail "a valid hostname was rejected"
grep -q $'^hostname = plutoplus-a\r$' "$CFG" || fail "valid hostname did not take"
echo "  valid hostname accepted"
echo "PASS"
