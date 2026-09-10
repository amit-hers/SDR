// sdr_bridge -- carry IP packets over the fabric QPSK modem. Runs ON the Pluto.
//
// The host reaches the radio over the Pluto's RJ45 (eth0) and simply wants
// traffic to go somewhere, so this is a routed hop: a TUN carrying layer-3
// packets, not a TAP carrying Ethernet frames. Nothing on the host needs a tap
// device of its own.
//
//   usage: sdr_bridge --local 172.30.99.1 --peer 172.30.99.2 [options]
//
// The two directions run in their own threads because both block, and for
// different reasons that must not be allowed to interact:
//
//  * The transmit write is paced by the DAC -- that is the link's flow control
//    and is wanted -- so a write of one frame parks the caller for as long as
//    the air takes. Reading the TUN from that same thread would stall the
//    receive path behind the transmitter.
//  * The receive read blocks until the packetizer's TLAST arrives, ~137 ms at
//    3.84 MS/s. Framing an outbound packet behind that would add the same
//    latency to every transmission.
//
// Build for the target with fpga/tools/build_bridge.sh -- it cross-compiles
// static for ARMv7 and needs neither liquid-dsp nor OpenSSL, because the
// framing path used here passes nullptr for both FEC and cipher.
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/OffsetDeframer.hpp"
#include "sdr/framing/Frame.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <linux/if.h>
#include <linux/if_tun.h>

using namespace sdr;

// ── Configuration ─────────────────────────────────────────────────────────
namespace {

struct Opts {
    std::string iface   = "sdr0";
    std::string local;                 // required
    std::string peer;                  // required
    int         prefix  = 30;          // /30 is exactly two hosts: one hop
    std::string tx_dev;                // resolved from name if empty
    std::string rx_dev;
    uint32_t    node_id = 1;
    int         mtu     = 1400;        // == MAX_PAYLOAD; TUN carries no L2 header
    int         pkt     = 32768;       // PKT_BYTES in axis_packetizer.v
    int         idle_ms = 200;         // keepalive cadence when there is no traffic
    bool        forward = false;
    std::string route;                 // network behind the peer, via the radio
    int         stats_s = 5;
};

void usage() {
    std::fprintf(stderr,
      "usage: sdr_bridge --local <ip> --peer <ip> [options]\n"
      "  --iface NAME     tun interface name (default sdr0)\n"
      "  --prefix N       prefix length (default 30)\n"
      "  --tx DEV         transmit char device (default: resolved by name)\n"
      "  --rx DEV         receive char device\n"
      "  --node-id N      this node's id; frames carrying it are ignored\n"
      "  --mtu N          default 1400, the framing layer's MAX_PAYLOAD\n"
      "  --pkt N          DMA packet size, must equal PKT_BYTES (default 32768)\n"
      "  --idle-ms N      keepalive cadence, 0 disables (default 200)\n"
      "  --route CIDR     a network behind the peer, routed over the radio\n"
      "  --forward        enable IPv4 forwarding (eth0 <-> radio)\n"
      "  --stats N        statistics interval in seconds, 0 disables\n");
}

// The IIO device indices are not stable -- they depend on which drivers probed,
// and the two boards in this lab disagree. Resolving by name is the only thing
// that holds; an index that has shifted sends the transmit stream into the
// RECEIVE device, which fails with EPERM and looks like a dead modulator.
std::string iioDevByName(const char* want) {
    for (int i = 0; i < 16; ++i) {
        char path[128];
        std::snprintf(path, sizeof path,
                      "/sys/bus/iio/devices/iio:device%d/name", i);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        char name[64] = {0};
        if (std::fgets(name, sizeof name, f)) {
            char* nl = std::strchr(name, '\n'); if (nl) *nl = 0;
            if (!std::strcmp(name, want)) {
                std::fclose(f);
                char dev[64];
                std::snprintf(dev, sizeof dev, "/dev/iio:device%d", i);
                return dev;
            }
        }
        std::fclose(f);
    }
    return {};
}

// The kernel counts what the interface discarded; asking it is more truthful
// than counting our own write() failures, which miss drops that happen after
// the packet is accepted.
uint64_t ifCounter(const std::string& iface, const char* what) {
    char path[160];
    std::snprintf(path, sizeof path,
                  "/sys/class/net/%s/statistics/%s", iface.c_str(), what);
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (std::fscanf(f, "%llu", &v) != 1) v = 0;
    std::fclose(f);
    return static_cast<uint64_t>(v);
}

int runCmd(const std::string& c) {
    int rc = std::system(c.c_str());
    return (rc == -1) ? -1 : WEXITSTATUS(rc);
}

} // namespace

// ── TUN ───────────────────────────────────────────────────────────────────
static int tunOpen(const std::string& name, int mtu) {
    int fd = ::open("/dev/net/tun", O_RDWR);
    if (fd < 0 && errno == ENOENT) {
        // The Pluto's rootfs is a minimal ramdisk and does not ship the node
        // even where the driver is built in, so create it rather than fail.
        // 10:200 is the fixed misc-device number for TUN.
        ::mkdir("/dev/net", 0755);
        if (::mknod("/dev/net/tun", S_IFCHR | 0600, makedev(10, 200)) == 0)
            fd = ::open("/dev/net/tun", O_RDWR);
    }
    if (fd < 0) {
        std::fprintf(stderr, "bridge: open /dev/net/tun: %s\n", std::strerror(errno));
        if (errno == ENOENT || errno == ENODEV)
            std::fprintf(stderr,
                "        This kernel has no TUN driver. The ADI 5.10 rootfs is\n"
                "        built without CONFIG_TUN; the Pluto+ 6.12.77 firmware\n"
                "        that the release flashes does have it. Flash that image.\n");
        return -1;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof ifr);
    // IFF_TUN: layer 3. IFF_NO_PI: no 4-byte packet-info prefix, so what is
    // read is the IP packet itself and can be framed without adjustment.
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
        std::perror("TUNSETIFF"); ::close(fd); return -1;
    }
    (void)mtu;
    return fd;
}

static bool tunConfigure(const Opts& o) {
    char cmd[512];
    // NetworkManager will remove an address it did not assign, usually within a
    // second or two of the link appearing, and afterwards packets for the peer
    // leave by the DEFAULT route instead -- silently, by the wrong interface,
    // looking exactly like a radio that is not transmitting. Ask it to leave
    // this interface alone. It is absent on the Pluto, so a failure here is
    // expected and not an error.
    std::snprintf(cmd, sizeof cmd,
                  "nmcli device set %s managed no >/dev/null 2>&1", o.iface.c_str());
    runCmd(cmd);

    std::snprintf(cmd, sizeof cmd, "ip link set %s mtu %d up", o.iface.c_str(), o.mtu);
    if (runCmd(cmd) != 0) { std::fprintf(stderr, "bridge: '%s' failed\n", cmd); return false; }

    // A point-to-point address, because that is what a radio hop is: it gives a
    // host route to the far end without a subnet's worth of assumptions.
    std::snprintf(cmd, sizeof cmd, "ip addr add %s peer %s/%d dev %s",
                  o.local.c_str(), o.peer.c_str(), o.prefix, o.iface.c_str());
    runCmd(cmd);   // EEXIST on restart is fine; verified below

    if (!o.route.empty()) {
        std::snprintf(cmd, sizeof cmd, "ip route replace %s via %s dev %s",
                      o.route.c_str(), o.peer.c_str(), o.iface.c_str());
        if (runCmd(cmd) != 0)
            std::fprintf(stderr, "bridge: route %s via radio FAILED\n", o.route.c_str());
        else
            std::fprintf(stderr, "bridge: route %s via radio: up\n", o.route.c_str());
    }

    if (o.forward) {
        if (runCmd("sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1") != 0)
            runCmd("echo 1 > /proc/sys/net/ipv4/ip_forward");
        std::fprintf(stderr, "bridge: IPv4 forwarding enabled\n");
    }

    // VERIFY the address survived. Checking is the whole point: an address that
    // was flushed leaves a link that looks up and carries nothing.
    std::snprintf(cmd, sizeof cmd,
                  "ip -4 addr show dev %s | grep -q 'inet %s'",
                  o.iface.c_str(), o.local.c_str());
    if (runCmd(cmd) != 0) {
        std::fprintf(stderr,
            "bridge: %s lost its address immediately after it was set.\n"
            "        Something is managing the interface; refusing to run, because\n"
            "        traffic would leave by the default route and the radio would\n"
            "        merely appear silent.\n", o.iface.c_str());
        return false;
    }
    std::fprintf(stderr, "bridge: %s %s peer %s/%d mtu %d\n",
                 o.iface.c_str(), o.local.c_str(), o.peer.c_str(), o.prefix, o.mtu);
    return true;
}

// ── Statistics ────────────────────────────────────────────────────────────
namespace {
struct Stats {
    std::atomic<uint64_t> tx_pkts{0}, tx_bytes{0}, tx_idle{0}, tx_err{0};
    std::atomic<uint64_t> rx_dma{0}, rx_frames{0}, rx_bytes{0};
    std::atomic<uint64_t> rx_crcerr{0}, rx_dup{0}, rx_ctrl{0}, rx_self{0};
    std::atomic<uint64_t> off_hits[4]{};

    // ── Overrun detection ────────────────────────────────────────────────
    // Whether the far end lost packets and whether THIS BOARD could not keep
    // up are different faults with the same symptom, and every time they have
    // been confused here the RF path was blamed for a software limit. These
    // separate them without needing to interpret a single dB.
    std::atomic<uint64_t> decode_us_total{0};  // time inside the four-offset decode
    std::atomic<uint64_t> decode_us_max{0};    // worst single packet
    // Longest wait between reads. Meaningful only under SUSTAINED traffic: on
    // an idle link it simply measures the keepalive cadence, and reads back as
    // idle_ms rather than as anything wrong.
    std::atomic<uint64_t> rx_gap_us_max{0};
    std::atomic<uint64_t> tx_stall_us_total{0};// time blocked writing to the DAC
    std::atomic<uint64_t> rx_short{0};         // reads not equal to a full packet
    std::atomic<uint64_t> tun_tx_drop{0}, tun_rx_drop{0};
};
Stats g_stats;
std::atomic<bool> g_run{true};
} // namespace

// ── Transmit: TUN -> Framer -> fabric modulator ───────────────────────────
static void txLoop(int tun_fd, int tx_fd, const Opts& o) {
    Framer framer;
    uint32_t seq = 0;
    std::vector<uint8_t> pkt(static_cast<size_t>(o.mtu));

    auto send = [&](const uint8_t* p, size_t n, uint8_t flags) {
        std::vector<uint8_t> wire =
            // BW_5 is the nearest code to the 4 MHz RF bandwidth the bring-up
            // scripts set. The field is descriptive -- nothing in this path
            // acts on it -- but it should not claim a width the radio is not
            // using.
            framer.encode(p, n, flags, ModCode::QPSK, BwCode::BW_5,
                          o.node_id, seq++, nullptr, nullptr);
        size_t done = 0;
        auto w0 = std::chrono::steady_clock::now();
        while (done < wire.size() && g_run.load()) {
            ssize_t w = ::write(tx_fd, wire.data() + done, wire.size() - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                g_stats.tx_err.fetch_add(1);
                return false;
            }
            done += static_cast<size_t>(w);
        }
        // Blocked time here is the DAC pacing us, which is normal and is the
        // link's flow control. It is recorded so that a transmitter starved by
        // something else -- a stalled core, a wedged device -- is visibly
        // different from one that is simply waiting for the air.
        g_stats.tx_stall_us_total.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - w0).count()));
        return true;
    };

    while (g_run.load()) {
        struct pollfd pfd { tun_fd, POLLIN, 0 };
        int timeout = o.idle_ms > 0 ? o.idle_ms : 1000;
        int pr = ::poll(&pfd, 1, timeout);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        if (pr == 0) {
            // Nothing to send. The demodulator's timing loop has nothing to
            // track through a silent carrier, so an idle link drifts out of
            // lock and the next real packet pays a full re-acquisition. A
            // keepalive costs one frame per idle_ms and keeps the loop fed.
            // FL_CTRL marks it as not user data; the receive side drops it
            // rather than handing an empty packet to the kernel.
            if (o.idle_ms > 0) {
                static const uint8_t ka[16] = {0};
                if (send(ka, sizeof ka, FL_CTRL)) g_stats.tx_idle.fetch_add(1);
            }
            continue;
        }

        ssize_t n = ::read(tun_fd, pkt.data(), pkt.size());
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        if (send(pkt.data(), static_cast<size_t>(n), 0)) {
            g_stats.tx_pkts.fetch_add(1);
            g_stats.tx_bytes.fetch_add(static_cast<uint64_t>(n));
        }
    }
}

// ── Receive: fabric demodulator -> Deframer -> TUN ────────────────────────
//
// The four-offset decode lives in sdr/framing/OffsetDeframer.hpp so that the
// bridge and its test exercise the same code rather than two copies of it.
static void rxLoop(int tun_fd, int rx_fd, const Opts& o) {
    std::vector<uint8_t> buf(static_cast<size_t>(o.pkt));
    OffsetDeframer deframer;
    uint64_t crc_seen = 0, dup_seen = 0;
    bool pkt_checked = false;

    auto last_read = std::chrono::steady_clock::now();
    while (g_run.load()) {
        ssize_t n = ::read(rx_fd, buf.data(), buf.size());
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { std::perror("read rx"); break; }
            continue;
        }
        auto now = std::chrono::steady_clock::now();
        // The gap between reads is the packet's airtime while we are keeping
        // up. A gap much longer than usual means a transfer went by while this
        // thread was still busy with the previous one -- the DMA does not wait.
        uint64_t gap = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - last_read).count());
        last_read = now;
        uint64_t gmax = g_stats.rx_gap_us_max.load();
        while (gap > gmax && !g_stats.rx_gap_us_max.compare_exchange_weak(gmax, gap)) {}
        g_stats.rx_dma.fetch_add(1);
        if (n != o.pkt) g_stats.rx_short.fetch_add(1);

        // The driver returns exactly one DMA packet per read, so the first read
        // reveals the real PKT_BYTES. A mismatch here is not cosmetic: decoding
        // is scoped to a transfer, and reading across a boundary was measured to
        // report 22.37% loss against a true 0.19%. Say so once, loudly, rather
        // than let it be chased as an RF fault -- which is what this class of
        // silent misconfiguration has cost before.
        if (!pkt_checked) {
            pkt_checked = true;
            if (n != o.pkt)
                std::fprintf(stderr,
                    "bridge: WARNING --pkt is %d but the DMA delivered %zd bytes.\n"
                    "        Pass --pkt %zd to match PKT_BYTES in axis_packetizer.v;\n"
                    "        decoding across a packet boundary loses frames silently.\n",
                    o.pkt, n, n);
        }

        // Time the decode. Compared against wall clock this gives the busy
        // fraction, which is the honest answer to "is the CPU the limit?" and
        // needs no knowledge of the sample rate to interpret.
        auto d0 = std::chrono::steady_clock::now();
        auto frames = deframer.pushPacket(buf.data(), static_cast<size_t>(n));
        uint64_t dus = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - d0).count());
        g_stats.decode_us_total.fetch_add(dus);
        uint64_t dmax = g_stats.decode_us_max.load();
        while (dus > dmax && !g_stats.decode_us_max.compare_exchange_weak(dmax, dus)) {}

        // The component keeps running totals; publish the deltas.
        uint64_t c = deframer.crcErrors(),  d = deframer.duplicates();
        g_stats.rx_crcerr.fetch_add(c - crc_seen); crc_seen = c;
        g_stats.rx_dup.fetch_add(d - dup_seen);    dup_seen = d;
        for (int i = 0; i < 4; ++i) g_stats.off_hits[i].store(deframer.offsetHits(i));

        for (auto& f : frames) {
            g_stats.rx_frames.fetch_add(1);
            // A keepalive is not user data; handing an empty or filler packet
            // to the kernel would be a bug visible only as junk on the wire.
            if (f.flags & FL_CTRL) { g_stats.rx_ctrl.fetch_add(1); continue; }
            if (f.payload.empty()) continue;

            ssize_t w = ::write(tun_fd, f.payload.data(), f.payload.size());
            if (w > 0) g_stats.rx_bytes.fetch_add(static_cast<uint64_t>(w));
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--iface")   o.iface   = next("--iface");
        else if (a == "--local")   o.local   = next("--local");
        else if (a == "--peer")    o.peer    = next("--peer");
        else if (a == "--prefix")  o.prefix  = std::atoi(next("--prefix").c_str());
        else if (a == "--tx")      o.tx_dev  = next("--tx");
        else if (a == "--rx")      o.rx_dev  = next("--rx");
        else if (a == "--node-id") o.node_id = static_cast<uint32_t>(std::strtoul(next("--node-id").c_str(), nullptr, 0));
        else if (a == "--mtu")     o.mtu     = std::atoi(next("--mtu").c_str());
        else if (a == "--pkt")     o.pkt     = std::atoi(next("--pkt").c_str());
        else if (a == "--idle-ms") o.idle_ms = std::atoi(next("--idle-ms").c_str());
        else if (a == "--route")   o.route   = next("--route");
        else if (a == "--forward") o.forward = true;
        else if (a == "--stats")   o.stats_s = std::atoi(next("--stats").c_str());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (o.local.empty() || o.peer.empty()) { usage(); return 2; }
    if (o.mtu > static_cast<int>(MAX_PAYLOAD)) {
        std::fprintf(stderr, "bridge: mtu %d exceeds MAX_PAYLOAD %zu; a packet that "
                             "large cannot be framed and would be dropped silently\n",
                     o.mtu, MAX_PAYLOAD);
        return 2;
    }

    if (o.tx_dev.empty()) o.tx_dev = iioDevByName("cf-ad9361-dds-core-lpc");
    if (o.rx_dev.empty()) o.rx_dev = iioDevByName("cf-ad9361-lpc");
    if (o.tx_dev.empty() || o.rx_dev.empty()) {
        std::fprintf(stderr, "bridge: could not resolve the IIO devices by name "
                             "(tx='%s' rx='%s'). Is the modem PL loaded?\n",
                     o.tx_dev.c_str(), o.rx_dev.c_str());
        return 1;
    }
    std::fprintf(stderr, "bridge: tx=%s rx=%s node=%u\n",
                 o.tx_dev.c_str(), o.rx_dev.c_str(), o.node_id);

    // The char devices are SINGLE-OPEN. Opening them before the interface is
    // configured means a stale holder is reported as itself rather than as a
    // confusing interface failure two steps later.
    int tx_fd = ::open(o.tx_dev.c_str(), O_WRONLY);
    if (tx_fd < 0) {
        std::fprintf(stderr, "bridge: open %s: %s\n", o.tx_dev.c_str(), std::strerror(errno));
        if (errno == EBUSY)
            std::fprintf(stderr, "        another process holds it; run free_capture_dev.sh\n");
        return 1;
    }
    int rx_fd = ::open(o.rx_dev.c_str(), O_RDONLY);
    if (rx_fd < 0) {
        std::fprintf(stderr, "bridge: open %s: %s\n", o.rx_dev.c_str(), std::strerror(errno));
        if (errno == EBUSY)
            std::fprintf(stderr, "        another process holds it; run free_capture_dev.sh\n");
        ::close(tx_fd); return 1;
    }

    int tun_fd = tunOpen(o.iface, o.mtu);
    if (tun_fd < 0) { ::close(tx_fd); ::close(rx_fd); return 1; }
    if (!tunConfigure(o)) { ::close(tun_fd); ::close(tx_fd); ::close(rx_fd); return 1; }

    std::thread tx(txLoop, tun_fd, tx_fd, std::cref(o));
    std::thread rx(rxLoop, tun_fd, rx_fd, std::cref(o));

    auto t_start = std::chrono::steady_clock::now();
    uint64_t last_decode = 0;
    bool     warned_short = false;
    while (g_run.load() && o.stats_s > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(o.stats_s));
        g_stats.tun_tx_drop.store(ifCounter(o.iface, "tx_dropped"));
        g_stats.tun_rx_drop.store(ifCounter(o.iface, "rx_dropped"));
        std::fprintf(stderr,
            "bridge: tx %llu pkts / %llu B (idle %llu, err %llu) | "
            "rx %llu dma, %llu frames, %llu B (crcerr %llu, dup %llu, ctrl %llu) | "
            "offsets %llu/%llu/%llu/%llu\n",
            (unsigned long long)g_stats.tx_pkts.load(),
            (unsigned long long)g_stats.tx_bytes.load(),
            (unsigned long long)g_stats.tx_idle.load(),
            (unsigned long long)g_stats.tx_err.load(),
            (unsigned long long)g_stats.rx_dma.load(),
            (unsigned long long)g_stats.rx_frames.load(),
            (unsigned long long)g_stats.rx_bytes.load(),
            (unsigned long long)g_stats.rx_crcerr.load(),
            (unsigned long long)g_stats.rx_dup.load(),
            (unsigned long long)g_stats.rx_ctrl.load(),
            (unsigned long long)g_stats.off_hits[0].load(),
            (unsigned long long)g_stats.off_hits[1].load(),
            (unsigned long long)g_stats.off_hits[2].load(),
            (unsigned long long)g_stats.off_hits[3].load());

        // ── Overrun verdict ──────────────────────────────────────────────
        // Printed as a conclusion, not as raw numbers. A reader who has to
        // work out whether 812000 us of decode in a 5 s window is a problem
        // will not do it at 2 a.m. while a link is down.
        uint64_t dtot = g_stats.decode_us_total.load();
        double   wall = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t_start).count();
        double   busy = wall > 0 ? (double)dtot / (wall * 1e6) * 100.0 : 0.0;
        double   busy_win = o.stats_s > 0
                          ? (double)(dtot - last_decode) / (o.stats_s * 1e6) * 100.0 : 0.0;
        last_decode = dtot;
        std::fprintf(stderr,
            "bridge: cpu decode %.1f%% now / %.1f%% avg (max %llu us/pkt) | "
            "rx gap max %llu us, short %llu | tx stall %llu ms | "
            "tun drops tx %llu rx %llu\n",
            busy_win, busy,
            (unsigned long long)g_stats.decode_us_max.load(),
            (unsigned long long)g_stats.rx_gap_us_max.load(),
            (unsigned long long)g_stats.rx_short.load(),
            (unsigned long long)(g_stats.tx_stall_us_total.load() / 1000),
            (unsigned long long)g_stats.tun_tx_drop.load(),
            (unsigned long long)g_stats.tun_rx_drop.load());
        if (busy_win > 70.0)
            std::fprintf(stderr,
                "bridge: WARNING decode is using %.0f%% of one core. Loss from here\n"
                "        is a CPU limit, NOT the radio -- do not chase it as RF.\n",
                busy_win);
        // Once, not every interval: this is a standing condition, and a
        // warning that repeats forever is one an operator learns to scroll
        // past.
        if (g_stats.rx_short.load() > 0 && g_stats.rx_dma.load() > 8 && !warned_short) {
            warned_short = true;
            std::fprintf(stderr,
                "bridge: WARNING %llu reads were not a full %d-byte packet; the byte\n"
                "        grid is only continuous within a transfer, so frames are\n"
                "        being lost at boundaries. Check --pkt against PKT_BYTES.\n",
                (unsigned long long)g_stats.rx_short.load(), o.pkt);
        }
    }
    if (o.stats_s <= 0) { tx.join(); rx.join(); }
    else { tx.detach(); rx.detach(); }
    return 0;
}
