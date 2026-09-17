#!/usr/bin/env python3
"""Phase 8 -- two-unit end-to-end datalink acceptance.

Drives the real product path and nothing else:

    Host-A -- eth -- UNIT-A -- RF -- UNIT-B -- eth -- Host-B

Run as root (raw sockets). Every measurement is taken from the bridges' own
counters plus what actually arrives at Host-B, never inferred.

    sudo tests/phase8/acceptance.py --a-iface enp3s0 --b-iface enx... \
         --unit-a 192.168.2.17 --unit-b 192.168.2.1 [--phase N]
"""
import argparse, binascii, os, re, socket, struct, subprocess, sys, time

ETH_P_ALL = 0x0003
TEST_ETHERTYPE = 0x88B5      # reserved for local experimental use
SSH = ["sshpass", "-p", "analog", "ssh", "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=10"]


def sh(host, cmd, timeout=60):
    r = subprocess.run(SSH + [f"root@{host}", cmd], capture_output=True,
                       text=True, timeout=timeout, stdin=subprocess.DEVNULL)
    return r.stdout.strip()


def mac_of(iface):
    with open(f"/sys/class/net/{iface}/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def open_raw(iface, rx_timeout=2.0):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((iface, 0))
    s.settimeout(rx_timeout)
    return s


# ---------------------------------------------------------------- phase 0
def identify(unit_a, unit_b):
    print("\n=== Phase 0: freeze / identify ===")
    out = {}
    for name, ip in (("UNIT-A", unit_a), ("UNIT-B", unit_b)):
        d = {
            "kernel":  sh(ip, "uname -r"),
            "fpga":    sh(ip, "devmem 0x43C50000 32; devmem 0x43C50004 32").replace("\n", " "),
            "bridge":  sh(ip, "md5sum /mnt/jffs2/sdr_bridge | cut -d' ' -f1"),
            "mtd3":    sh(ip, "head -c 8 /dev/mtd3 | od -An -tx1 | tr -d ' '"),
        }
        out[name] = d
        print(f"  {name}: kernel={d['kernel']} fpga={d['fpga']}")
        print(f"          bridge={d['bridge']} mtd3={d['mtd3']}")
    return out


# ------------------------------------------------------- the confound gate
def confound_check(a_iface, b_iface, unit_a, unit_b):
    """Refuse to certify anything until the RF link is the ONLY path.

    The two RJ45 ports were once cabled to each other. On that topology every
    Ethernet test passes with the radios powered off, so a PASS would mean
    nothing. This is checked by silencing UNIT-B's radio and proving that a
    frame injected at Host-A then CANNOT arrive -- if it still does, it came
    over copper.
    """
    print("\n=== Phase 1 gate: is the RF link the only path? ===")
    saved = sh(unit_b, "devmem 0x43C00010 32")
    sh(unit_b, "devmem 0x43C00010 32 0")          # demodulator off
    time.sleep(1)
    try:
        tx = open_raw(a_iface)
        rx = open_raw(b_iface, rx_timeout=3.0)
        magic = b"CONFOUND" + os.urandom(16)
        frame = (mac_of(b_iface) + mac_of(a_iface)
                 + struct.pack("!H", TEST_ETHERTYPE) + magic)
        frame += b"\x00" * max(0, 60 - len(frame))
        for _ in range(8):
            tx.send(frame)
        deadline = time.time() + 3.0
        leaked = False
        while time.time() < deadline:
            try:
                pkt = rx.recv(2048)
            except socket.timeout:
                break
            if magic in pkt:
                leaked = True
                break
    finally:
        sh(unit_b, f"devmem 0x43C00010 32 {saved}")   # restore

    if leaked:
        print("  FAIL: a frame crossed with UNIT-B's demodulator DISABLED.")
        print("        There is a non-RF path between the two ports (a direct")
        print("        RJ45 cable, or a shared switch). Remove it: with it in")
        print("        place every later result is meaningless, and the two")
        print("        bridges also form a loop -- A transmits over RF, B")
        print("        injects onto the wire, copper carries it back to A.")
        return False
    print("  PASS: nothing crossed with the radio disabled.")
    return True


# ---------------------------------------------------------------- counters
CTR = re.compile(
    r"tx (\d+) pkts / (\d+) B \(idle (\d+), err (\d+)\).*?"
    r"rx (\d+) dma, (\d+) frames, (\d+) B \(crcerr (\d+), dup (\d+), ctrl (\d+)\)")


def counters(ip):
    line = sh(ip, "grep -E '^bridge: tx ' /tmp/appliance.log | tail -1")
    m = CTR.search(line)
    if not m:
        return {}
    k = ("tx_pkts", "tx_bytes", "idle", "err",
         "rx_dma", "rx_frames", "rx_bytes", "crcerr", "dup", "ctrl")
    c = dict(zip(k, (int(x) for x in m.groups())))
    ov = sh(ip, "grep -oE 'oversize [0-9]+' /tmp/appliance.log | tail -1")
    c["oversize"] = int(ov.split()[-1]) if ov else 0
    return c


def idle_baseline(unit_a, unit_b):
    print("\n=== Phase 2: idle state ===")
    ok = True
    for name, ip in (("UNIT-A", unit_a), ("UNIT-B", unit_b)):
        n = sh(ip, "n=0; for d in /proc/[0-9]*; do e=$(readlink $d/exe 2>/dev/null)||"
                   "continue; case \"$e\" in */sdr_bridge*) n=$((n+1));; esac; done; echo $n")
        kids = sh(ip, "ps w | grep -cE '[i]io_readdev|[i]io_writedev'")
        c = counters(ip)
        print(f"  {name}: bridges={n} iio_children={kids} "
              f"err={c.get('err','?')} oversize={c.get('oversize','?')} rx_dma={c.get('rx_dma','?')}")
        if n != "1":
            print(f"    FAIL: expected exactly 1 bridge on {name}")
            ok = False
    return ok


# ------------------------------------------------- phase 5: byte-exactness
def frame_exact(a_iface, b_iface, payload=b"PHASE8-EXACTNESS-" + b"\xa5" * 64):
    print("\n=== Phase 5: RJ45 -> RF -> RJ45, byte-exact ===")
    src, dst = mac_of(a_iface), mac_of(b_iface)
    frame = dst + src + struct.pack("!H", TEST_ETHERTYPE) + payload
    tx, rx = open_raw(a_iface), open_raw(b_iface, rx_timeout=5.0)
    for _ in range(5):
        tx.send(frame)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            pkt = rx.recv(4096)
        except socket.timeout:
            break
        if payload in pkt:
            got = pkt[:len(frame)]
            checks = {
                "dst MAC":   got[0:6] == dst,
                "src MAC":   got[6:12] == src,
                "EtherType": got[12:14] == struct.pack("!H", TEST_ETHERTYPE),
                "payload":   got[14:14 + len(payload)] == payload,
                "length":    len(got) == len(frame),
            }
            for k, v in checks.items():
                print(f"  {k:10s}: {'OK' if v else 'MISMATCH'}")
            return all(checks.values())
    print("  FAIL: frame never arrived at Host-B")
    return False


# ------------------------------------------------ phase 6: protocol matrix
def protocol_matrix(a_iface, b_iface):
    print("\n=== Phase 6: protocol matrix ===")
    src, dst = mac_of(a_iface), mac_of(b_iface)
    bcast = b"\xff" * 6
    cases = {
        "ARP":             (bcast, 0x0806, b"\x00\x01\x08\x00\x06\x04\x00\x01" + src + b"\x0a\x63\x00\x01" + b"\x00" * 6 + b"\x0a\x63\x00\x02"),
        "IPv4":            (dst, 0x0800, b"\x45" + b"\x00" * 19),
        "IPv6":            (dst, 0x86DD, b"\x60" + b"\x00" * 39),
        "broadcast":       (bcast, TEST_ETHERTYPE, b"BROADCAST" + b"\xb1" * 40),
        "custom ethertype": (dst, 0x88B6, b"CUSTOM" + b"\xc7" * 40),
    }
    tx, rx = open_raw(a_iface), open_raw(b_iface, rx_timeout=3.0)
    results = {}
    for name, (d, et, body) in cases.items():
        tag = body[:8]
        frame = d + src + struct.pack("!H", et) + body
        frame += b"\x00" * max(0, 60 - len(frame))
        for _ in range(5):
            tx.send(frame)
        seen, deadline = False, time.time() + 3.0
        while time.time() < deadline:
            try:
                pkt = rx.recv(4096)
            except socket.timeout:
                break
            if tag in pkt and pkt[12:14] == struct.pack("!H", et):
                seen = True
                break
        results[name] = seen
        print(f"  {name:18s}: {'PASS' if seen else 'FAIL'}")
    return results


# ---------------------------------------------- phase 10: frame-size matrix
def frame_size_matrix(a_iface, b_iface, mtu=1400):
    print("\n=== Phase 10: frame-size matrix ===")
    src, dst = mac_of(a_iface), mac_of(b_iface)
    tx, rx = open_raw(a_iface), open_raw(b_iface, rx_timeout=3.0)
    results = {}
    for size in (46, 512, 1000, 1200, 1399, 1400, 1500):
        tag = f"SZ{size:05d}".encode()
        body = tag + os.urandom(max(0, size - len(tag)))
        frame = dst + src + struct.pack("!H", TEST_ETHERTYPE) + body
        for _ in range(4):
            try:
                tx.send(frame)
            except OSError as e:
                results[size] = f"send refused ({e.errno})"
                break
        else:
            got, deadline = None, time.time() + 3.0
            while time.time() < deadline:
                try:
                    pkt = rx.recv(4096)
                except socket.timeout:
                    break
                if tag in pkt:
                    got = pkt
                    break
            if got is None:
                # Over the payload limit this is the CORRECT outcome, not a bug:
                # the frame must be dropped cleanly rather than truncated.
                results[size] = "dropped" if size > mtu else "LOST"
            else:
                exact = got[14:14 + len(body)] == body
                results[size] = "byte-exact" if exact else "TRUNCATED/CORRUPT"
        print(f"  {size:5d} B: {results[size]}")
    return results


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--a-iface", required=True, help="Host-A NIC, cabled to UNIT-A RJ45")
    p.add_argument("--b-iface", required=True, help="Host-B NIC, cabled to UNIT-B RJ45")
    p.add_argument("--unit-a", default="192.168.2.17")
    p.add_argument("--unit-b", default="192.168.2.1")
    p.add_argument("--skip-confound-check", action="store_true",
                   help="DANGEROUS: only if the RF path is proven sole by other means")
    a = p.parse_args()

    if os.geteuid() != 0:
        sys.exit("must run as root (raw sockets)")

    identify(a.unit_a, a.unit_b)

    if not a.skip_confound_check:
        if not confound_check(a.a_iface, a.b_iface, a.unit_a, a.unit_b):
            sys.exit("\nABORT: a non-RF path exists. Nothing below would mean anything.")

    ok_idle = idle_baseline(a.unit_a, a.unit_b)
    before_a, before_b = counters(a.unit_a), counters(a.unit_b)

    ok_exact = frame_exact(a.a_iface, a.b_iface)
    proto = protocol_matrix(a.a_iface, a.b_iface)
    sizes = frame_size_matrix(a.a_iface, a.b_iface)

    after_a, after_b = counters(a.unit_a), counters(a.unit_b)
    print("\n=== counter deltas ===")
    for name, b4, af in (("UNIT-A", before_a, after_a), ("UNIT-B", before_b, after_b)):
        d = {k: af.get(k, 0) - b4.get(k, 0) for k in af}
        print(f"  {name}: " + " ".join(f"{k}=+{v}" for k, v in d.items() if v))

    print("\n=== SUMMARY ===")
    print(f"  idle state            : {'PASS' if ok_idle else 'FAIL'}")
    print(f"  ethernet byte-exact   : {'PASS' if ok_exact else 'FAIL'}")
    print(f"  protocol matrix       : {sum(proto.values())}/{len(proto)} passed")
    bad = [s for s, r in sizes.items() if r in ("LOST", "TRUNCATED/CORRUPT")]
    print(f"  frame-size matrix     : {'PASS' if not bad else 'FAIL at ' + str(bad)}")


if __name__ == "__main__":
    main()
