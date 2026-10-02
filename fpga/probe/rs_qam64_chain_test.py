#!/usr/bin/env python3
"""64-QAM counterpart of fpga/probe/rs_qam16_chain_test.py -- see that
script's header for the full rationale (first end-to-end hardware proof
that RS FEC and QAM modulation work together, not just separately).

The byte<->symbol packing is DIFFERENT from 16-QAM's: a 16-QAM symbol
carries exactly 4 bits (one nibble = one symbol, 2 symbols per byte,
trivial), but a 64-QAM symbol carries 6 bits, which does not divide evenly
into an 8-bit byte. Since lcm(6, 8) = 24, every 3 consecutive codeword bytes
(24 bits) pack into exactly 4 consecutive 6-bit symbols -- and since
255 = 3 * 85, the full codeword packs into a whole number of groups with no
leftover bits, so no padding scheme is needed.

Runs against a combined bitstream (adi-hdl/projects/rs_harness/, extended
with axi_qam64_test_harness_hw.v as a fourth peripheral) with four
independent AXI-Lite harnesses:
    0x43C60000  per-block RS harness (SELECT=0 is rs_encoder)
    0x43C70000  integrated RS decoder core (rs_decoder.v)
    0x43C80000  16-QAM mapper/demapper harness
    0x43C90000  64-QAM mapper/demapper harness

Usage:
    python3 fpga/probe/rs_qam64_chain_test.py --host root@192.168.2.1 --password analog
    python3 fpga/probe/rs_qam64_chain_test.py --host root@192.168.2.1 --password analog --corrupt-symbols 3
"""
import argparse
import random
import subprocess
import sys
import tempfile
import os

ENCODE_SH = r"""#!/bin/sh
set -e
BASE=$((0x43C60000))
SELECT=$((BASE+0x04)); PUSH=$((BASE+0x08)); STATUS=$((BASE+0x0C)); POP=$((BASE+0x10))
QBASE=$((0x43C90000))
PUSH_SYM=$((QBASE+0x04)); MAP_STATUS=$((QBASE+0x08)); POP_I=$((QBASE+0x0C)); POP_Q=$((QBASE+0x10))

devmem $SELECT w 0   # rs_encoder

# rs_encoder is a ZERO-DEPTH PASSTHROUGH (see fpga/rtl/rs_encoder.v's own
# header, and the hard lesson from the 16-QAM chain test's first attempt):
# it will NOT accept byte N+1 until byte N's output is popped. Push and pop
# MUST be interleaved for the first 223 bytes; the remaining 32 parity bytes
# then drain with no more pushing needed.
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

# Pack 3 codeword bytes (24 bits) into 4 six-bit symbols at a time.
> /tmp/chain_iq.hex
cnt=0
b0=0; b1=0; b2=0
while read -r b; do
  hb=$((0x$b))
  if   [ $cnt -eq 0 ]; then b0=$hb; cnt=1
  elif [ $cnt -eq 1 ]; then b1=$hb; cnt=2
  else
    b2=$hb; cnt=0
    v=$(( (b0<<16) | (b1<<8) | b2 ))
    s0=$(( (v>>18) & 0x3F )); s1=$(( (v>>12) & 0x3F ))
    s2=$(( (v>>6)  & 0x3F )); s3=$(( v       & 0x3F ))
    for sym in $s0 $s1 $s2 $s3; do
      while true; do st=$(devmem $MAP_STATUS); r=$((st&1)); [ $r -ne 0 ] && break; done
      devmem $PUSH_SYM w $sym
      while true; do st=$(devmem $MAP_STATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
      iv=$(devmem $POP_I)
      qv=$(devmem $POP_Q)
      printf '%04x %04x\n' $((iv & 0xFFFF)) $((qv & 0xFFFF)) >> /tmp/chain_iq.hex
    done
  fi
done < /tmp/chain_codeword.hex
echo DONE
"""

DECODE_SH = r"""#!/bin/sh
set -e
QBASE=$((0x43C90000))
PUSH_I=$((QBASE+0x14)); PUSH_Q=$((QBASE+0x18)); DEMAP_STATUS=$((QBASE+0x1C))
POP_SYM=$((QBASE+0x20)); POP_ERROR=$((QBASE+0x24))
DBASE=$((0x43C70000))
DPUSH=$((DBASE+0x04)); DSTATUS=$((DBASE+0x08)); DPOP=$((DBASE+0x0C)); DERR=$((DBASE+0x10))

> /tmp/chain_recon.hex
cnt=0
s0=0; s1=0; s2=0
while read -r ihex qhex; do
  while true; do st=$(devmem $DEMAP_STATUS); r=$((st&1)); [ $r -ne 0 ] && break; done
  devmem $PUSH_I w 0x$ihex
  devmem $PUSH_Q w 0x$qhex
  while true; do st=$(devmem $DEMAP_STATUS); mv=$(((st>>1)&1)); [ $mv -ne 0 ] && break; done
  sym=$(devmem $POP_SYM)
  devmem $POP_ERROR > /dev/null
  symv=$((sym & 0x3F))
  if   [ $cnt -eq 0 ]; then s0=$symv; cnt=1
  elif [ $cnt -eq 1 ]; then s1=$symv; cnt=2
  elif [ $cnt -eq 2 ]; then s2=$symv; cnt=3
  else
    s3=$symv; cnt=0
    v=$(( (s0<<18) | (s1<<12) | (s2<<6) | s3 ))
    b0=$(( (v>>16) & 0xFF )); b1=$(( (v>>8) & 0xFF )); b2=$(( v & 0xFF ))
    printf '%02x\n%02x\n%02x\n' $b0 $b1 $b2 >> /tmp/chain_recon.hex
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
        print("Phase 1+2: RS-encoding and 64-QAM-mapping 223 bytes (340 symbols) on real hardware...", file=sys.stderr)
        ssh(args.host, args.password, "sh /tmp/chain_encode.sh")

        iq_path = os.path.join(work, "iq.hex")
        scp_from(args.host, args.password, "/tmp/chain_iq.hex", iq_path)

        with open(iq_path) as f:
            iq_lines = [line.split() for line in f if line.strip()]
        assert len(iq_lines) == 340, f"expected 340 symbol pairs, got {len(iq_lines)}"

        if args.corrupt_symbols:
            corrupted_indices = rng.sample(range(340), args.corrupt_symbols)
            for idx in corrupted_indices:
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
        print("Phase 3+4: 64-QAM-demapping and RS-decoding on real hardware...", file=sys.stderr)
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
