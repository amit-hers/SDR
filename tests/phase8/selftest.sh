#!/usr/bin/env bash
# Validate the acceptance harness against a veth pair, with no radio involved.
#
# The harness is the instrument for Phase 8. An unvalidated instrument is how
# this project has repeatedly reached confident wrong conclusions: a probe that
# read zero because the chain was stalled, an analysis that filtered out the one
# status line distinguishing "never armed" from "saw nothing", a PER of 0.00%
# reported over zero capturable frames. So the harness gets checked against a
# link whose answer is known before it is trusted on one whose answer is not.
#
# A veth pair is a perfect transparent L2 link, so every measurement must pass.
# It is also, by construction, a non-RF path -- which makes it the right way to
# prove the confound gate CATCHES a leak rather than merely claiming it would.
#
#   sudo tests/phase8/selftest.sh
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "must run as root (raw sockets, veth)" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cleanup() { ip link del vphA 2>/dev/null || true; }
trap cleanup EXIT
cleanup
ip link add vphA type veth peer name vphB
ip link set vphA up; ip link set vphB up
ip link set vphA mtu 1500; ip link set vphB mtu 1500

python3 - "$ROOT" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/tests/phase8")
import acceptance as A

fail = 0
ok_exact = A.frame_exact("vphA", "vphB")
proto    = A.protocol_matrix("vphA", "vphB")
sizes    = A.frame_size_matrix("vphA", "vphB", mtu=1500)

# The gate must REFUSE here: a veth pair is exactly the non-RF path it exists to
# catch. Board access is stubbed so a running appliance is not disturbed.
A.sh = lambda host, cmd, timeout=60: "0x00000001"
gate = A.confound_check("vphA", "vphB", "stub-a", "stub-b")

print("\n=== self-test verdict ===")
for name, good in (("byte-exact", ok_exact),
                   ("protocol matrix", all(proto.values())),
                   ("frame-size matrix", all(v == "byte-exact" for v in sizes.values())),
                   ("confound gate catches a leak", gate is False)):
    print(f"  {name:32s}: {'PASS' if good else 'FAIL'}")
    fail += 0 if good else 1
sys.exit(1 if fail else 0)
PY
