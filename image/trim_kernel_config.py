import sys, re

src, dst = sys.argv[1], sys.argv[2]

# Disabled: nothing here is reachable on a Pluto+ acting as a modem. WiFi and
# sound have no hardware behind them at all; netfilter/NFS are services this
# image does not run. Everything the product needs is in KEEP below and is
# asserted afterwards, so a careless prefix match cannot silently remove it.
DISABLE_PREFIX = [
 "CONFIG_WIRELESS", "CONFIG_CFG80211", "CONFIG_MAC80211", "CONFIG_WLAN",
 "CONFIG_RFKILL", "CONFIG_LIB80211", "CONFIG_BT",
 "CONFIG_SOUND", "CONFIG_SND", "CONFIG_AC97",
 "CONFIG_NETFILTER", "CONFIG_IP_NF", "CONFIG_IP6_NF", "CONFIG_NF_",
 "CONFIG_BRIDGE_NF", "CONFIG_NETFILTER_XT",
 "CONFIG_NFS", "CONFIG_NFSD", "CONFIG_SUNRPC", "CONFIG_RPCSEC",
 "CONFIG_CIFS", "CONFIG_BTRFS", "CONFIG_XFS", "CONFIG_F2FS", "CONFIG_REISERFS",
 "CONFIG_OCFS2", "CONFIG_GFS2", "CONFIG_NILFS",
 "CONFIG_DRM", "CONFIG_FB", "CONFIG_BACKLIGHT", "CONFIG_LOGO", "CONFIG_VGA",
 "CONFIG_MEDIA", "CONFIG_VIDEO", "CONFIG_DVB",
 "CONFIG_INFINIBAND", "CONFIG_CAN", "CONFIG_ATM", "CONFIG_IRDA",
 "CONFIG_HAMRADIO", "CONFIG_6LOWPAN", "CONFIG_IEEE802154", "CONFIG_NFC",
 "CONFIG_WIMAX", "CONFIG_RDS", "CONFIG_TIPC", "CONFIG_SCTP", "CONFIG_DCCP",
 "CONFIG_L2TP", "CONFIG_MPLS", "CONFIG_NET_SCH", "CONFIG_NET_CLS",
 "CONFIG_SCSI_", "CONFIG_ISCSI", "CONFIG_MD_", "CONFIG_DM_", "CONFIG_RAID",
]
# Never disabled, whatever the prefixes above would match.
KEEP = {
 "CONFIG_TUN":"y", "CONFIG_MACB":"y", "CONFIG_USB_CONFIGFS":"y",
 "CONFIG_AD9361":"y", "CONFIG_CF_AXI_ADC":"y", "CONFIG_CF_AXI_DDS":"y",
 "CONFIG_ARCH_ZYNQ":"y", "CONFIG_XILINX_WATCHDOG":"y",
 "CONFIG_JFFS2_FS":"y", "CONFIG_MTD":"y", "CONFIG_MTD_SPI_NOR":"y",
 "CONFIG_IIO_BUFFER":"y", "CONFIG_IIO_BUFFER_DMAENGINE":"y",
 "CONFIG_NET":"y", "CONFIG_INET":"y", "CONFIG_UNIX":"y", "CONFIG_PACKET":"y",
}
lines = open(src).read().splitlines()
out, disabled = [], []
for ln in lines:
    m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", ln)
    if m:
        sym, val = m.group(1), m.group(2)
        if sym in KEEP:
            out.append(ln); continue
        if any(sym.startswith(p) for p in DISABLE_PREFIX):
            out.append("# %s is not set" % sym); disabled.append(sym); continue
    out.append(ln)

# Assert the product-critical symbols survived.
text = "\n".join(out)
bad = [k for k, v in KEEP.items()
       if not re.search(r"^%s=%s$" % (re.escape(k), re.escape(v)), text, re.M)]
open(dst, "w").write(text + "\n")
print("    disabled %d symbols" % len(disabled))
for p in ["CONFIG_MAC80211","CONFIG_SND","CONFIG_NETFILTER","CONFIG_NFS","CONFIG_DRM"]:
    n = len([s for s in disabled if s.startswith(p)])
    if n: print("      %-22s %d" % (p+"*", n))
if bad:
    print("    ERROR: product-critical symbols lost: %s" % ", ".join(bad)); sys.exit(1)
print("    all product-critical symbols intact (TUN, MACB, AD9361, USB gadget, MTD/JFFS2)")
