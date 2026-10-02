#!/usr/bin/env python3
"""First end-to-end hardware proof that RS(255,223) FEC and 16-QAM
modulation work TOGETHER, not just separately. Every prior hardware test
this session proved one piece in isolation (the RS decoder corrects
synthetic byte errors; the 16-QAM mapper/demapper round-trips correctly,
including under injected AWGN) -- this chains the REAL hardware blocks
end-to-end: RS encode -> 16-QAM map -> (optional injected symbol errors,
simulating a lossy channel) -> 16-QAM demap -> RS decode, and checks the
final output against the original payload.

Runs against a single combined bitstream (adi-hdl/projects/rs_harness/, this
round extended with axi_qam16_test_harness_hw.v as a third peripheral) with
three independent AXI-Lite harnesses:
    0x43C60000  per-block RS harness (SELECT=0 is rs_encoder)
    0x43C70000  integrated RS decoder core (rs_decoder.v)
    0x43C80000  16-QAM mapper/demapper harness

All orchestration happens on the HOST (this script) -- no new RTL exists to
combine these blocks autonomously in hardware yet; that would be the next,
separate step. Each phase here drives one already-individually-proven
hardware block.

Protocol per codeword:
    1. RS-encode 223 random payload bytes -> 255-byte codeword (hardware).
    2. 16-QAM-map each codeword byte as two nibbles (hi, then lo) ->
       510 (I,Q) symbol pairs (hardware).
    3. Optionally corrupt some symbol pairs in SOFTWARE, simulating channel
       errors a real lossy link would introduce.
    4. 16-QAM-demap each (I,Q) pair back to a nibble, reassemble into bytes
       -> a (possibly corrupted) reconstructed codeword (hardware).
    5. RS-decode the reconstructed codeword -> 223 bytes + fail/error_count
       (hardware).
    6. Compare against the original payload.

Usage:
    python3 fpga/probe/rs_qam16_chain_test.py --host root@192.168.2.1 --password analog
    python3 fpga/probe/rs_qam16_chain_test.py --host root@192.168.2.1 --password analog --corrupt-symbols 3
"""
import argparse
import random
import subprocess
import sys
import tempfile
import os

RS_HARNESS_BASE = 0x43C60000   # PUSH=+0x08 STATUS=+0x0C POP=+0x10, SELECT=+0x04 (0=encoder)
DECODER_BASE    = 0x43C70000   # PUSH=+0x04 STATUS=+0x08 POP=+0x0C ERROR_COUNT=+0x10
QAM16_BASE      = 0x43C80000   # PUSH_SYM=+0x04 MAP_STATUS=+0x08 POP_I=+0x0C POP_Q=+0x10
                                # PUSH_I=+0x14 PUSH_Q=+0x18 DEMAP_STATUS=+0x1C POP_SYM=+0x20 POP_ERROR=+0x24

ENCODE_SH = r"""#!/bin/sh
set -e
BASE=$((0x43C60000))
SELECT=$((BASE+0x04)); PUSH=$((BASE+0x08)); STATUS=$((BASE+0x0C)); POP=$((BASE+0x10))
QBASE=$((0x43C80000))
PUSH_SYM=$((QBASE+0x04)); MAP_STATUS=$((QBASE+0x08)); POP_I=$((QBASE+0x0C)); POP_Q=$((QBASE+0x10))

devmem $SELECT w 0   # rs_encoder

# rs_encoder is a ZERO-DEPTH PASSTHROUGH (established earlier this session,
# see fpga/rtl/rs_encoder.v's own header): each accepted input byte's output
# becomes valid immediately, so it will NOT accept byte N+1 until byte N's
# output has been popped. Pushing all 223 input bytes in one loop before
# popping anything (an earlier version of this script did exactly that)
# deadlocks permanently after the very first byte -- s_ready never returns,
# since nothing drains the one-byte output register it's waiting on. Push
# and pop must be interleaved for the first 223 bytes; the remaining 32
# parity bytes then drain with no more pushing needed (input exhausted).
> /tmp/chain_codeword.hex
while read -r b; do
  while true; do st=$(devmem $STATUS); r=$((st&1)); [ $r -ne 0 ] && break; done
  devmem $PUSH w 0x$b
  while true; do st=$(devmem $STATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
  v=$(devmem $POP)
  printf '%02x\n' $((v)) >> /tmp/chain_codeword.hex
done < /tmp/chain_payload.hex
i=0
while [ $i -lt 32 ]; do
  while true; do st=$(devmem $STATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
  v=$(devmem $POP)
  printf '%02x\n' $((v)) >> /tmp/chain_codeword.hex
  i=$((i+1))
done

> /tmp/chain_iq.hex
while read -r b; do
  hi=$(( (0x$b >> 4) & 0xF ))
  lo=$(( 0x$b & 0xF ))
  for nib in $hi $lo; do
    while true; do st=$(devmem $MAP_STATUS); r=$((st&1)); [ $r -ne 0 ] && break; done
    devmem $PUSH_SYM w $nib
    while true; do st=$(devmem $MAP_STATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
    iv=$(devmem $POP_I)
    qv=$(devmem $POP_Q)
    printf '%04x %04x\n' $((iv & 0xFFFF)) $((qv & 0xFFFF)) >> /tmp/chain_iq.hex
  done
done < /tmp/chain_codeword.hex
echo DONE
"""

DECODE_SH = r"""#!/bin/sh
set -e
QBASE=$((0x43C80000))
PUSH_I=$((QBASE+0x14)); PUSH_Q=$((QBASE+0x18)); DEMAP_STATUS=$((QBASE+0x1C))
POP_SYM=$((QBASE+0x20)); POP_ERROR=$((QBASE+0x24))
DBASE=$((0x43C70000))
DPUSH=$((DBASE+0x04)); DSTATUS=$((DBASE+0x08)); DPOP=$((DBASE+0x0C)); DERR=$((DBASE+0x10))

> /tmp/chain_recon.hex
nib_hi=""
while read -r ihex qhex; do
  while true; do st=$(devmem $DEMAP_STATUS); r=$((st&1)); [ $r -ne 0 ] && break; done
  devmem $PUSH_I w 0x$ihex
  devmem $PUSH_Q w 0x$qhex
  while true; do st=$(devmem $DEMAP_STATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
  nib=$(devmem $POP_SYM)
  devmem $POP_ERROR > /dev/null
  if [ -z "$nib_hi" ]; then
    nib_hi=$nib
  else
    byte=$(( (nib_hi<<4) | nib ))
    printf '%02x\n' $byte >> /tmp/chain_recon.hex
    nib_hi=""
  fi
done < /tmp/chain_iq.hex

while read -r b; do
  while true; do st=$(devmem $DSTATUS); r=$((st&1)); [ $r -ne 0 ] && break; done
  devmem $DPUSH w 0x$b
done < /tmp/chain_recon.hex

> /tmp/chain_result.hex
i=0
fail=""
ec=""
while [ $i -lt 223 ]; do
  while true; do st=$(devmem $DSTATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
  if [ $i -eq 222 ]; then
    mlast=$(((st>>2)&1))
    fail=$(((st>>3)&1))
    ec=$(devmem $DERR)
  fi
  v=$(devmem $DPOP)
  printf '%02x\n' $((v)) >> /tmp/chain_result.hex
  i=$((i+1))
done
echo "fail=$fail error_count=$ec"
echo DONE
"""


def ssh(host, password, cmd, **kw):
    return subprocess.run(["sshpass", "-p", password, "ssh",
                            "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password",
                            "-o", "ConnectTimeout=8", host, cmd],
                           check=True, capture_output=True, text=True, **kw)


def scp_to(host, password, src, dst):
    subprocess.run(["sshpass", "-p", password, "scp", "-O",
                     "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password",
                     src, f"{host}:{dst}"], check=True)


def scp_from(host, password, src, dst):
    subprocess.run(["sshpass", "-p", password, "scp", "-O",
                     "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password",
                     f"{src}", dst], check=True) if False else \
        subprocess.run(["sshpass", "-p", password, "scp", "-O",
                         "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password",
                         f"{host}:{src}", dst], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="root@192.168.2.1")
    ap.add_argument("--password", default="analog")
    ap.add_argument("--corrupt-symbols", type=int, default=0,
                    help="number of (I,Q) symbol pairs to corrupt between map and demap, simulating channel errors")
    ap.add_argument("--seed", type=int, default=640016)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    payload = [rng.randrange(256) for _ in range(223)]

    with tempfile.TemporaryDirectory() as work:
        payload_path = os.path.join(work, "payload.hex")
        with open(payload_path, "w") as f:
            for b in payload:
                f.write(f"{b:02x}\n")

        encode_sh = os.path.join(work, "encode.sh")
        with open(encode_sh, "w") as f:
            f.write(ENCODE_SH)
        decode_sh = os.path.join(work, "decode.sh")
        with open(decode_sh, "w") as f:
            f.write(DECODE_SH)

        print("Uploading payload + encode script...", file=sys.stderr)
        scp_to(args.host, args.password, payload_path, "/tmp/chain_payload.hex")
        scp_to(args.host, args.password, encode_sh, "/tmp/chain_encode.sh")
        print("Phase 1+2: RS-encoding and 16-QAM-mapping 223 bytes (510 symbols) on real hardware...", file=sys.stderr)
        ssh(args.host, args.password, "sh /tmp/chain_encode.sh")

        iq_path = os.path.join(work, "iq.hex")
        scp_from(args.host, args.password, "/tmp/chain_iq.hex", iq_path)
        codeword_path = os.path.join(work, "codeword.hex")
        scp_from(args.host, args.password, "/tmp/chain_codeword.hex", codeword_path)

        with open(iq_path) as f:
            iq_lines = [line.split() for line in f if line.strip()]
        assert len(iq_lines) == 510, f"expected 510 symbol pairs, got {len(iq_lines)}"

        corrupted_indices = []
        if args.corrupt_symbols:
            corrupted_indices = rng.sample(range(510), args.corrupt_symbols)
            for idx in corrupted_indices:
                # flip to a strongly wrong constellation region: negate and
                # shift, guaranteed to land in a different decision region
                i_val = int(iq_lines[idx][0], 16)
                i_signed = i_val - 0x10000 if i_val >= 0x8000 else i_val
                corrupted = max(-32768, min(32767, -i_signed))
                iq_lines[idx][0] = f"{corrupted & 0xFFFF:04x}"
            with open(iq_path, "w") as f:
                for i_hex, q_hex in iq_lines:
                    f.write(f"{i_hex} {q_hex}\n")
            print(f"Corrupted {args.corrupt_symbols} symbol(s) at indices {sorted(corrupted_indices)}", file=sys.stderr)

        print("Uploading (possibly corrupted) symbols + decode script...", file=sys.stderr)
        scp_to(args.host, args.password, iq_path, "/tmp/chain_iq.hex")
        scp_to(args.host, args.password, decode_sh, "/tmp/chain_decode.sh")
        print("Phase 3+4: 16-QAM-demapping and RS-decoding on real hardware...", file=sys.stderr)
        result = ssh(args.host, args.password, "sh /tmp/chain_decode.sh")
        print(result.stdout.strip(), file=sys.stderr)

        result_path = os.path.join(work, "result.hex")
        scp_from(args.host, args.password, "/tmp/chain_result.hex", result_path)
        ssh(args.host, args.password,
            "rm -f /tmp/chain_payload.hex /tmp/chain_codeword.hex /tmp/chain_iq.hex /tmp/chain_recon.hex "
            "/tmp/chain_result.hex /tmp/chain_encode.sh /tmp/chain_decode.sh")

        with open(result_path) as f:
            recovered = [int(line.strip(), 16) for line in f if line.strip()]

    assert len(recovered) == 223, f"expected 223 recovered bytes, got {len(recovered)}"
    mismatches = sum(1 for a, b in zip(payload, recovered) if a != b)
    print(f"\nPayload recovered correctly: {'YES' if mismatches == 0 else 'NO'} "
          f"({mismatches} byte mismatches out of 223)")
    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
