import os, sys, glob, shutil, subprocess, re

root = os.path.abspath(sys.argv[1])

DROP_GLOBS = [
 "root/iqengine", "opt/vfat.img",
 "usr/bin/maia*", "lib/modules/*/updates/maia-sdr.ko", "etc/maia*",
 "usr/sbin/hostapd*", "usr/sbin/wpa_*", "usr/bin/wpa_*",
 "usr/sbin/gpsd*", "usr/bin/gps*", "usr/sbin/mosquitto*", "usr/bin/mosquitto*",
 "usr/sbin/avahi-*", "usr/bin/avahi-*", "usr/lib/avahi*",
 "usr/bin/dbus-*", "usr/lib/dbus*", "etc/dbus-1",
 "usr/lib/SoapySDR", "usr/bin/Soapy*",
 "usr/sbin/chronyd", "usr/bin/chronyc",
 "usr/lib/xtables", "usr/sbin/iptables*", "usr/sbin/xtables*",
 "usr/bin/input-event-daemon", "usr/sbin/rpc.*", "usr/sbin/nfs*",
 "usr/share/doc", "usr/share/man", "usr/share/locale", "usr/share/alsa",
 "usr/share/gdb", "usr/share/icu",
 "usr/bin/openssl", "usr/bin/curl", "usr/bin/hostapd*", "usr/bin/opkg*",
 "usr/lib/libopkg*", "usr/lib/ossl-modules", "usr/bin/xmlcatalog", "usr/bin/xmllint",
 "usr/bin/alsa*", "usr/bin/amixer", "usr/bin/aplay", "usr/bin/arecord", "usr/bin/aserver",
 "usr/sbin/alsactl", "usr/bin/speaker-test", "usr/bin/aconnect", "usr/bin/aseqdump",
 "usr/lib/opkg", "usr/share/opkg",
 "root/watchdatveasy.sh", "root/watchconsoletx.sh", "root/watchconsolefreq.sh",
 "root/api_controller.sh", "root/lnb_config.sh", "root/install_maia.sh",
 "root/plutotx", "root/sweep", "root/sweep.sh", "root/scan_fm.sh",
 "root/waterfall", "root/*.js", "usr/lib/libcivetweb*", "lib/modules", "root/websrv",
 "root/spy_external_sdr.sh", "root/img", "root/index.html", "root/paho-mqtt-min.js",
 "bin/bash",
]
DROP_INITD = ["S30dbus-daemon","S35iptables","S45msd","S49chronyd","S50avahi-daemon",
              "S05avahi-setup.sh","S50gpsd","S50mosquitto","S50maia-kmod",
              "S50maia-sdr-certificates","S60maia-httpd","S91nfs-mount",
              "S99input-event-daemon", "S95bgcript"]

# Kept unconditionally: the runtime our own tools need. sdr_bridge/sdrctl live
# in jffs2, not in this tree, so the closure below cannot see that they are
# C++ binaries -- omitting libstdc++ would strand them at boot.
FORCE = ["libc.so", "libm.so", "libgcc_s", "libpthread", "librt.so",
         "libatomic", "ld-linux", "libnss_", "libresolv", "libdl.so", "libutil.so",
         "libiio.so", "libusb", "libaio", "libserialport", "libz.so", "libcrypt.so"]

def rm(p):
    if os.path.islink(p) or os.path.isfile(p): os.unlink(p)
    elif os.path.isdir(p): shutil.rmtree(p)

def treesize(r):
    return sum(os.path.getsize(os.path.join(d,f))
               for d,_,fs in os.walk(r) for f in fs
               if not os.path.islink(os.path.join(d,f)))

before = treesize(root)
for g in DROP_GLOBS:
    for p in glob.glob(os.path.join(root, g)): rm(p)
for s in DROP_INITD:
    p = os.path.join(root, "etc/init.d", s)
    if os.path.exists(p): rm(p)

def is_elf(p):
    try:
        with open(p,'rb') as f: return f.read(4) == b'\x7fELF'
    except OSError: return False

# DT_NEEDED via readelf: binutils reads ARM ELF fine on an x86 host, and gets
# the program-header field order right, which a hand-rolled parser did not.
def needed_map(paths):
    out = {}
    B = 200
    for i in range(0, len(paths), B):
        batch = paths[i:i+B]
        try:
            r = subprocess.run(["readelf","-d"]+batch, capture_output=True, text=True)
        except Exception: return out
        cur = batch[0] if len(batch) == 1 else None
        for line in r.stdout.splitlines():
            m = re.match(r"^File:\s*(.+)$", line.strip())
            if m: cur = m.group(1).strip(); continue
            m = re.search(r"\(NEEDED\).*\[(.+?)\]", line)
            if m and cur: out.setdefault(cur, []).append(m.group(1))
    return out

LIBDIRS = ["lib", "usr/lib"]
libindex = {}
for d in LIBDIRS:
    dp = os.path.join(root, d)
    if os.path.isdir(dp):
        for f in os.listdir(dp):
            libindex.setdefault(f, os.path.join(dp, f))

all_elf = []
for d, _, fs in os.walk(root):
    for f in fs:
        p = os.path.join(d, f)
        if not os.path.islink(p) and is_elf(p): all_elf.append(p)
NEED = needed_map(all_elf)

def in_libdir(p):
    rel = os.path.relpath(p, root)
    return any(rel.startswith(x+"/") for x in LIBDIRS)

roots = [p for p in all_elf if not in_libdir(p)]
for f, tgt in libindex.items():
    if any(a in f for a in FORCE): roots.append(os.path.realpath(tgt))

keep, stack = set(), list(roots)
while stack:
    p = stack.pop()
    for n in NEED.get(p, []):
        tgt = libindex.get(n)
        if not tgt: continue
        real = os.path.realpath(tgt)
        if real in keep: continue
        keep.add(real); stack.append(real)

dropped = []
for d in LIBDIRS:
    dp = os.path.join(root, d)
    if not os.path.isdir(dp): continue
    for f in sorted(os.listdir(dp)):
        p = os.path.join(dp, f)
        if os.path.isdir(p): continue
        if any(a in f for a in FORCE): continue
        real = os.path.realpath(p)
        if real not in keep and (os.path.islink(p) or is_elf(p)):
            dropped.append((f, 0 if os.path.islink(p) else os.path.getsize(p)))
            os.unlink(p)

# Dropping /bin/bash saves ~1 MB, but root's login shell is /bin/bash: dropbear
# then authenticates and has nothing to exec, so every ssh session closes with
# no output and no error. Repoint root at busybox sh.
import io
pw = os.path.join(root, "etc/passwd")
if os.path.exists(pw) and not os.path.exists(os.path.join(root, "bin/bash")):
    lines = open(pw).read().splitlines()
    out2, changed = [], 0
    for ln in lines:
        if ln.startswith("root:") and ln.endswith("/bin/bash"):
            ln = ln[: -len("/bin/bash")] + "/bin/sh"; changed += 1
        out2.append(ln)
    open(pw, "w").write("\n".join(out2) + "\n")
    print("    /etc/passwd: repointed %d shell(s) from /bin/bash to /bin/sh" % changed)

# --- validation gate: every surviving binary must resolve every NEEDED -------
have = set()
for d in LIBDIRS:
    dp = os.path.join(root, d)
    if os.path.isdir(dp): have |= set(os.listdir(dp))
missing = {}
for d, _, fs in os.walk(root):
    for f in fs:
        p = os.path.join(d, f)
        if os.path.islink(p) or not is_elf(p): continue
        for n in NEED.get(p, []):
            if n not in have: missing.setdefault(os.path.relpath(p, root), []).append(n)

after = treesize(root)
print("    libraries removed: %d" % len(dropped))
for f, s in sorted(dropped, key=lambda x:-x[1])[:10]:
    print("      %-32s %8d B" % (f, s))
print("    tree: %.1f MB -> %.1f MB" % (before/1e6, after/1e6))
if missing:
    print("    BROKEN -- unresolved dependencies:")
    for k, v in list(missing.items())[:20]:
        print("      %-40s needs %s" % (k, ", ".join(v)))
    sys.exit(1)
print("    validation: all NEEDED libraries resolve")
