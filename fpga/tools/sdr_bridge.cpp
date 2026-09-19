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
#include <mutex>
#include "sdr/bridge/LoopGuard.hpp"
#include "sdr/framing/PacketReader.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/framing/DmaBlockAggregator.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <csignal>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>
// Present in the kernel since 4.20; the userspace header may predate it.
#ifndef PACKET_IGNORE_OUTGOING
#define PACKET_IGNORE_OUTGOING 23
#endif
#include <linux/if_ether.h>
#include <sys/socket.h>
#include <arpa/inet.h>

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
    // Explicit --tx/--rx means "use these paths directly", which is how the
    // end-to-end test drives the bridge over FIFOs with no radio present.
    // Left unset, the transfers go through libiio, which is the only path that
    // actually programs the DMA on this kernel.
    bool        dev_explicit = false;
    // Use one synchronous libiio TX buffer instead of feeding iio_writedev
    // through a pipe. This prevents unsent keepalives from accumulating ahead
    // of newly arrived user data while preserving a continuous sample stream.
    bool        direct_iio_tx = false;
    bool        direct_iio_rx = false;
    // AF_PACKET carries whole Ethernet frames off a real NIC; TUN carries IP
    // packets on a virtual one. The product wants the former (RJ45 to RJ45 with
    // nothing configured on either PC), and the 5.10 kernel that actually
    // transmits has no CONFIG_TUN, so this is the default path on hardware.
    bool        raw_eth = false;
    uint32_t    node_id = 1;
    int         mtu     = 1400;        // == MAX_PAYLOAD; TUN carries no L2 header
    int         pkt     = 32768;       // PKT_BYTES in axis_packetizer.v
    int         tx_block = 0;          // direct-IIO TX bytes; 0 uses pkt
    int         tx_qlen = 32;          // bound bulk backlog without dropping interactive traffic
    int         idle_ms = 200;         // keepalive cadence when there is no traffic
    int         batch_us = 3000;       // collect TUN packets before a data-block flush
    bool        forward = false;
    std::string route;                 // network behind the peer, via the radio
    int         stats_s = 5;
};

void usage() {
    std::fprintf(stderr,
      "usage: sdr_bridge --local <ip> --peer <ip> [options]\n"
      "  --iface NAME     tun interface name (default sdr0)\n"
      "  --raw-eth IFACE  carry Ethernet frames off IFACE via AF_PACKET instead\n"
      "                   of creating a TUN. No IP is configured on the radio;\n"
      "                   the two RJ45 ports behave as one cable. Needs no\n"
      "                   CONFIG_TUN, which the working 5.10 kernel lacks.\n"
      "  --prefix N       prefix length (default 30)\n"
      "  --tx DEV         transmit char device (default: resolved by name)\n"
      "  --rx DEV         receive char device\n"
      "  --direct-iio-tx  synchronous single-buffer libiio TX (experimental)\n"
      "  --direct-iio-rx  synchronous single-buffer libiio RX (experimental)\n"
      "  --node-id N      this node's id; frames carrying it are ignored\n"
      "  --mtu N          default 1400, the framing layer's MAX_PAYLOAD\n"
      "  --pkt N          DMA packet size, must equal PKT_BYTES (default 32768)\n"
      "  --tx-block N     direct-IIO TX block bytes (default: same as --pkt)\n"
      "  --tx-qlen N      TUN transmit queue length in packets (default 32)\n"
      "  --idle-ms N      keepalive cadence, 0 disables (default 200)\n"
      "  --batch-us N     TX aggregation window in microseconds (default 3000)\n"
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


// ── libiio, not the char device ────────────────────────────────────────────
//
// On the Pluto+ 6.12.77 kernel a raw write()/read() on /dev/iio:deviceN NEVER
// programs the DMA: ctrl, flags and x_length stay at their reset values, no
// interrupt ever fires, and the call blocks forever while every register reads
// healthy. Measured on one board minutes apart -- raw write gave an all-zero
// modulator output, libiio gave 64/64 non-zero with interrupts firing, and on
// receive libiio returned 524288 B in 4 s where a raw read never returned.
//
// So the transfers go through iio_writedev / iio_readdev, spawned as children
// with a pipe. That keeps this binary static and dependency-free -- linking
// libiio would mean cross-compiling it for the target -- and uses exactly the
// path proven to work on the hardware. At ~1 MB/s the extra copy is irrelevant.
static pid_t g_tx_pid = -1, g_rx_pid = -1;

// Spawn a child with one end of a pipe as its stdin (to_child) or stdout.
static int spawnIio(const char* const argv[], bool to_child, pid_t* pid_out) {
    int fds[2];
    if (::pipe(fds) < 0) { std::perror("pipe"); return -1; }
    // Do not let continuous keepalive blocks build a deep queue in front of a
    // newly arrived data packet.  One DMA block already takes about 120 ms at
    // the product's 3.84 MS/s setting; a multi-block pipe turns that into
    // user-visible latency in each direction.  RX is unaffected: its pipe must
    // retain the kernel default so a brief decode delay does not discard data.
#ifdef F_SETPIPE_SZ
    if (to_child) {
        const int dma_block_bytes = 32768;
        (void)::fcntl(fds[1], F_SETPIPE_SZ, dma_block_bytes);
    }
#endif
    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); ::close(fds[0]); ::close(fds[1]); return -1; }
    if (pid == 0) {
        if (to_child) { ::dup2(fds[0], STDIN_FILENO);  ::close(fds[1]); }
        else          { ::dup2(fds[1], STDOUT_FILENO); ::close(fds[0]); }
        ::close(fds[0]); ::close(fds[1]);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
        ::execvp(argv[0], const_cast<char* const*>(argv));
        ::_exit(127);
    }
    *pid_out = pid;
    if (to_child) { ::close(fds[0]); return fds[1]; }
    ::close(fds[1]); return fds[0];
}

// Minimal runtime binding to the libiio 0.x ABI already shipped by the board.
// Keeping the types opaque avoids importing target headers into the host build,
// and dlopen keeps FIFO-based tests independent of libiio being installed.
class DirectIioTx {
    struct iio_context;
    struct iio_device;
    struct iio_channel;
    struct iio_buffer;

    void* so_ = nullptr;
    iio_context* ctx_ = nullptr;
    iio_buffer* buf_ = nullptr;
    size_t bytes_ = 0;

    using CreateContext = iio_context* (*)();
    using DestroyContext = void (*)(iio_context*);
    using FindDevice = iio_device* (*)(const iio_context*, const char*);
    using FindChannel = iio_channel* (*)(const iio_device*, const char*, bool);
    using EnableChannel = void (*)(iio_channel*);
    using CreateBuffer = iio_buffer* (*)(const iio_device*, size_t, bool);
    using DestroyBuffer = void (*)(iio_buffer*);
    using BufferPtr = void* (*)(const iio_buffer*);
    using BufferPush = ssize_t (*)(iio_buffer*);

    DestroyContext destroy_context_ = nullptr;
    DestroyBuffer destroy_buffer_ = nullptr;
    BufferPtr buffer_start_ = nullptr;
    BufferPtr buffer_end_ = nullptr;
    BufferPush buffer_push_ = nullptr;

    template <class T> bool symbol(T& out, const char* name) {
        out = reinterpret_cast<T>(::dlsym(so_, name));
        if (out) return true;
        std::fprintf(stderr, "bridge: libiio missing %s: %s\n", name, ::dlerror());
        return false;
    }

public:
    ~DirectIioTx() {
        if (buf_ && destroy_buffer_) destroy_buffer_(buf_);
        if (ctx_ && destroy_context_) destroy_context_(ctx_);
        if (so_) ::dlclose(so_);
    }

    bool open(size_t samples, size_t expected_bytes) {
        so_ = ::dlopen("libiio.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so_) {
            std::fprintf(stderr, "bridge: dlopen(libiio.so.0): %s\n", ::dlerror());
            return false;
        }
        CreateContext create_context = nullptr;
        FindDevice find_device = nullptr;
        FindChannel find_channel = nullptr;
        EnableChannel enable_channel = nullptr;
        CreateBuffer create_buffer = nullptr;
        if (!symbol(create_context, "iio_create_local_context") ||
            !symbol(destroy_context_, "iio_context_destroy") ||
            !symbol(find_device, "iio_context_find_device") ||
            !symbol(find_channel, "iio_device_find_channel") ||
            !symbol(enable_channel, "iio_channel_enable") ||
            !symbol(create_buffer, "iio_device_create_buffer") ||
            !symbol(destroy_buffer_, "iio_buffer_destroy") ||
            !symbol(buffer_start_, "iio_buffer_start") ||
            !symbol(buffer_end_, "iio_buffer_end") ||
            !symbol(buffer_push_, "iio_buffer_push")) return false;

        ctx_ = create_context();
        if (!ctx_) { std::fprintf(stderr, "bridge: cannot create local IIO context\n"); return false; }
        iio_device* dev = find_device(ctx_, "cf-ad9361-dds-core-lpc");
        if (!dev) { std::fprintf(stderr, "bridge: TX IIO device not found\n"); return false; }
        iio_channel* ch0 = find_channel(dev, "voltage0", true);
        iio_channel* ch1 = find_channel(dev, "voltage1", true);
        if (!ch0 || !ch1) { std::fprintf(stderr, "bridge: TX IIO channels not found\n"); return false; }
        enable_channel(ch0);
        enable_channel(ch1);
        buf_ = create_buffer(dev, samples, false);
        if (!buf_) {
            std::fprintf(stderr, "bridge: cannot create direct TX IIO buffer: %s\n",
                         std::strerror(errno));
            return false;
        }
        auto* begin = static_cast<uint8_t*>(buffer_start_(buf_));
        auto* end = static_cast<uint8_t*>(buffer_end_(buf_));
        bytes_ = static_cast<size_t>(end - begin);
        if (bytes_ != expected_bytes) {
            std::fprintf(stderr, "bridge: direct TX buffer is %zu B, expected %zu B\n",
                         bytes_, expected_bytes);
            return false;
        }
        return true;
    }

    ssize_t push(const uint8_t* data, size_t size) {
        if (!buf_ || size != bytes_) { errno = EINVAL; return -1; }
        std::memcpy(buffer_start_(buf_), data, size);
        return buffer_push_(buf_);
    }
};

class DirectIioRx {
    struct iio_context;
    struct iio_device;
    struct iio_channel;
    struct iio_buffer;

    void* so_ = nullptr;
    iio_context* ctx_ = nullptr;
    iio_buffer* buf_ = nullptr;
    size_t bytes_ = 0;

    using CreateContext = iio_context* (*)();
    using DestroyContext = void (*)(iio_context*);
    using FindDevice = iio_device* (*)(const iio_context*, const char*);
    using FindChannel = iio_channel* (*)(const iio_device*, const char*, bool);
    using EnableChannel = void (*)(iio_channel*);
    using CreateBuffer = iio_buffer* (*)(const iio_device*, size_t, bool);
    using DestroyBuffer = void (*)(iio_buffer*);
    using BufferPtr = void* (*)(const iio_buffer*);
    using BufferRefill = ssize_t (*)(iio_buffer*);

    DestroyContext destroy_context_ = nullptr;
    DestroyBuffer destroy_buffer_ = nullptr;
    BufferPtr buffer_start_ = nullptr;
    BufferPtr buffer_end_ = nullptr;
    BufferRefill buffer_refill_ = nullptr;

    template <class T> bool symbol(T& out, const char* name) {
        out = reinterpret_cast<T>(::dlsym(so_, name));
        if (out) return true;
        std::fprintf(stderr, "bridge: libiio missing %s: %s\n", name, ::dlerror());
        return false;
    }

public:
    ~DirectIioRx() {
        if (buf_ && destroy_buffer_) destroy_buffer_(buf_);
        if (ctx_ && destroy_context_) destroy_context_(ctx_);
        if (so_) ::dlclose(so_);
    }

    bool open(size_t samples, size_t expected_bytes) {
        so_ = ::dlopen("libiio.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so_) {
            std::fprintf(stderr, "bridge: dlopen(libiio.so.0): %s\n", ::dlerror());
            return false;
        }
        CreateContext create_context = nullptr;
        FindDevice find_device = nullptr;
        FindChannel find_channel = nullptr;
        EnableChannel enable_channel = nullptr;
        CreateBuffer create_buffer = nullptr;
        if (!symbol(create_context, "iio_create_local_context") ||
            !symbol(destroy_context_, "iio_context_destroy") ||
            !symbol(find_device, "iio_context_find_device") ||
            !symbol(find_channel, "iio_device_find_channel") ||
            !symbol(enable_channel, "iio_channel_enable") ||
            !symbol(create_buffer, "iio_device_create_buffer") ||
            !symbol(destroy_buffer_, "iio_buffer_destroy") ||
            !symbol(buffer_start_, "iio_buffer_start") ||
            !symbol(buffer_end_, "iio_buffer_end") ||
            !symbol(buffer_refill_, "iio_buffer_refill")) return false;

        ctx_ = create_context();
        if (!ctx_) { std::fprintf(stderr, "bridge: cannot create local IIO context\n"); return false; }
        iio_device* dev = find_device(ctx_, "cf-ad9361-lpc");
        if (!dev) { std::fprintf(stderr, "bridge: RX IIO device not found\n"); return false; }
        iio_channel* ch0 = find_channel(dev, "voltage0", false);
        iio_channel* ch1 = find_channel(dev, "voltage1", false);
        if (!ch0 || !ch1) { std::fprintf(stderr, "bridge: RX IIO channels not found\n"); return false; }
        enable_channel(ch0);
        enable_channel(ch1);
        buf_ = create_buffer(dev, samples, false);
        if (!buf_) {
            std::fprintf(stderr, "bridge: cannot create direct RX IIO buffer: %s\n",
                         std::strerror(errno));
            return false;
        }
        auto* begin = static_cast<uint8_t*>(buffer_start_(buf_));
        auto* end = static_cast<uint8_t*>(buffer_end_(buf_));
        bytes_ = static_cast<size_t>(end - begin);
        if (bytes_ != expected_bytes) {
            std::fprintf(stderr, "bridge: direct RX buffer is %zu B, expected %zu B\n",
                         bytes_, expected_bytes);
            return false;
        }
        return true;
    }

    ssize_t refill(uint8_t* data, size_t size) {
        if (!buf_ || size != bytes_) { errno = EINVAL; return -1; }
        ssize_t n = buffer_refill_(buf_);
        if (n <= 0) return n;
        if (static_cast<size_t>(n) > size) { errno = EOVERFLOW; return -1; }
        std::memcpy(data, buffer_start_(buf_), static_cast<size_t>(n));
        return n;
    }
};

// Opening the transmit buffer re-points the DAC channel at the internal DDS, so
// the DMA source select must be redone AFTER the writer is running. Done here
// through /dev/mem rather than by shelling out to devmem, so the bridge stays
// self-contained. Silently skipped if /dev/mem is unavailable -- the caller's
// bring-up script may already have handled it.
static void selectDmaSource() {
    int fd = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return;
    const off_t base = 0x79024000;
    const size_t len = 0x1000;
    void* m = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (m != MAP_FAILED) {
        volatile uint32_t* r = static_cast<volatile uint32_t*>(m);
        for (int ch = 0; ch < 2; ++ch) r[(0x418 + 64 * ch) / 4] = 2;   // source = DMA
        ::munmap(m, len);
    }
    ::close(fd);
}

// Read one 32-bit PL register without depending on an external devmem utility.
// Identity validation is deliberately best-effort: an unprivileged diagnostic
// invocation may not have /dev/mem, while a readable identity which disagrees
// with the requested packet size is an unsafe, hard error.
static bool readPhys32(off_t address, uint32_t& value) {
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const off_t page_mask = static_cast<off_t>(page_size - 1);
    const off_t page = address & ~page_mask;
    const size_t offset = static_cast<size_t>(address - page);
    int fd = ::open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) return false;
    void* m = ::mmap(nullptr, static_cast<size_t>(page_size), PROT_READ,
                     MAP_SHARED, fd, page);
    if (m == MAP_FAILED) { ::close(fd); return false; }
    volatile const uint32_t* reg = reinterpret_cast<volatile const uint32_t*>(
        static_cast<const uint8_t*>(m) + offset);
    value = *reg;
    ::munmap(m, static_cast<size_t>(page_size));
    ::close(fd);
    return true;
}

static bool verifyFpgaPacketBytes(int requested) {
    constexpr off_t identity = 0x43C50000;
    constexpr uint32_t magic_expected = 0x5344524C;
    uint32_t magic = 0;
    if (!readPhys32(identity, magic)) {
        std::fprintf(stderr,
                     "bridge: WARNING cannot read FPGA identity; --pkt cannot be verified\n");
        return true;
    }
    if (magic != magic_expected) {
        std::fprintf(stderr,
                     "bridge: FPGA identity mismatch: magic=0x%08x, expected 0x%08x\n",
                     magic, magic_expected);
        return false;
    }

    uint32_t packet_bytes = 0;
    if (!readPhys32(identity + 0x18, packet_bytes) || packet_bytes == 0) {
        std::fprintf(stderr,
                     "bridge: WARNING FPGA identity predates RX_PKT_BYTES; --pkt=%d is unverified\n",
                     requested);
        return true;
    }
    if (packet_bytes != static_cast<uint32_t>(requested)) {
        std::fprintf(stderr,
                     "bridge: packet-size mismatch: FPGA RX_PKT_BYTES=%u, --pkt=%d; refusing to start\n",
                     packet_bytes, requested);
        return false;
    }
    std::fprintf(stderr, "bridge: verified FPGA RX_PKT_BYTES=%u\n", packet_bytes);
    return true;
}

struct DemodStatus {
    uint32_t lock_count = 0;
    uint32_t mu_clamped = 0;
    bool valid = false;
};

// Read the two diagnostics used to distinguish a live-but-stalled demodulator
// from a dead DMA path.  Keep this separate from the data-plane mapping: a
// failure to open /dev/mem must disable recovery, never stop the bridge.
static DemodStatus demodStatus() {
    DemodStatus s;
    int fd = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return s;
    const off_t base = 0x43C00000;
    const size_t len = 0x1000;
    void* m = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (m != MAP_FAILED) {
        volatile uint32_t* r = static_cast<volatile uint32_t*>(m);
        s.lock_count = r[0x18 / 4];
        s.mu_clamped = r[0x30 / 4];
        s.valid = true;
        ::munmap(m, len);
    }
    ::close(fd);
    return s;
}

// The receive thread and iio_readdev remain active while this pulse is issued,
// so the demodulator output is drained.  That is essential: a backpressured
// HLS core cannot observe its AXI-Lite soft_reset input.
static bool demodSoftResetDrained() {
    int fd = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return false;
    const off_t base = 0x43C00000;
    const size_t len = 0x1000;
    void* m = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (m == MAP_FAILED) { ::close(fd); return false; }
    volatile uint32_t* r = static_cast<volatile uint32_t*>(m);
    r[0x20 / 4] = 1;
    ::usleep(100 * 1000);
    r[0x20 / 4] = 0;
    ::munmap(m, len);
    ::close(fd);
    return true;
}


// ── AF_PACKET: carry Ethernet frames straight off the RJ45 ────────────────
//
// The alternative to a TUN device, and the better fit for the product: a PC
// plugs into each radio's Ethernet port and the two behave like one cable. No
// IP is configured on the radios, nothing on the PCs needs to know an SDR is
// involved, and ARP, DHCP and IPv6 all cross unmodified because whole L2
// frames are carried rather than IP packets.
//
// It also needs no kernel change. The Pluto's 5.10 kernel -- the only one whose
// AD9363 transmitter actually works -- is built without CONFIG_TUN, and getting
// a rebuilt kernel to boot was not achievable without UART access to u-boot.
// AF_PACKET is in every kernel.
//
// ETH_P_ALL in the protocol field captures every frame the interface sees,
// including ones the host stack will also process. That is deliberate: the
// radio is a wire, not an endpoint, and filtering here would silently drop
// protocols someone later depends on.
static int packetOpen(const std::string& iface, int mtu) {
    int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { std::fprintf(stderr, "bridge: socket(AF_PACKET): %s\n", std::strerror(errno)); return -1; }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof ifr);
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        std::fprintf(stderr, "bridge: no such interface '%s': %s\n", iface.c_str(), std::strerror(errno));
        ::close(fd); return -1;
    }
    const int ifindex = ifr.ifr_ifindex;

    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof sll);
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = ifindex;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sll), sizeof sll) < 0) {
        std::fprintf(stderr, "bridge: bind %s: %s\n", iface.c_str(), std::strerror(errno));
        ::close(fd); return -1;
    }

    // Promiscuous: without it the NIC drops frames not addressed to it, so
    // traffic between the two PCs would never be seen -- the radio would carry
    // only what was addressed to the radio itself, which is nothing useful.
    struct packet_mreq mr;
    std::memset(&mr, 0, sizeof mr);
    mr.mr_ifindex = ifindex;
    mr.mr_type    = PACKET_MR_PROMISC;
    if (::setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr) < 0)
        std::fprintf(stderr, "bridge: WARNING could not set promiscuous mode on %s: %s\n",
                     iface.c_str(), std::strerror(errno));

    // Do NOT capture the frames this process itself injects.
    //
    // An ETH_P_ALL socket is delivered outbound frames too (sll_pkttype ==
    // PACKET_OUTGOING), so every packet decoded off the radio and written to
    // this interface came straight back in and was transmitted again. On a
    // two-unit link that is an unconditional loop: A sends a frame, B injects
    // it on its own wire, B's own capture socket sees that injection and sends
    // it back to A, forever. It is self-sustaining -- it needs no broadcast
    // traffic to start and does not decay, because every lap is regenerated at
    // full power by the modem.
    //
    // This is not the protocol filtering rejected above. Nothing addressed to
    // anyone is dropped; only this process's own echo is, and no peer can ever
    // legitimately need that. The direction comes from the kernel, so there is
    // no source-MAC heuristic to get wrong and frames that genuinely repeat a
    // MAC still cross.
    const int ignore_outgoing = 1;
    if (::setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING,
                     &ignore_outgoing, sizeof ignore_outgoing) < 0)
        std::fprintf(stderr, "bridge: WARNING could not ignore outgoing frames on %s: %s\n"
                             "bridge:         (needs kernel 4.20+) -- injected frames will loop\n",
                     iface.c_str(), std::strerror(errno));

    // Bring the link up. No address: this is a wire, not a host.
    //
    // The MTU has to be set with the link DOWN. macb rejects SIOCSIFMTU on a
    // running interface with EBUSY, and combining both in one "ip link set up
    // mtu N" leaves the MTU untouched while the command still reports success
    // under 2>/dev/null. The interface then sits at 1500 while this code
    // believes it is at MAX_PAYLOAD, so a full-size 1514-byte frame from an
    // attached camera or PC arrives, exceeds the payload limit and is counted
    // as oversize and dropped -- silently, and only for the largest frames,
    // which is the traffic a video feed is mostly made of.
    char cmd[256];
    std::snprintf(cmd, sizeof cmd, "ip link set %s down 2>/dev/null", iface.c_str());
    runCmd(cmd);
    std::snprintf(cmd, sizeof cmd, "ip link set %s mtu %d 2>/dev/null", iface.c_str(), mtu);
    runCmd(cmd);
    std::snprintf(cmd, sizeof cmd, "ip link set %s up 2>/dev/null", iface.c_str());
    runCmd(cmd);

    // Report what the interface ACTUALLY carries, not what was asked for.
    // Printing the requested value is how the failure above stayed invisible.
    int actual_mtu = -1;
    {
        char path[128];
        std::snprintf(path, sizeof path, "/sys/class/net/%s/mtu", iface.c_str());
        if (FILE* f = std::fopen(path, "r")) {
            if (std::fscanf(f, "%d", &actual_mtu) != 1) actual_mtu = -1;
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "bridge: AF_PACKET on %s (ifindex %d, promiscuous, mtu %d)\n",
                 iface.c_str(), ifindex, actual_mtu);
    if (actual_mtu > mtu)
        std::fprintf(stderr,
                     "bridge: WARNING %s mtu is %d but the radio carries %d; frames larger\n"
                     "bridge:         than that will be counted oversize and dropped\n",
                     iface.c_str(), actual_mtu, mtu);
    return fd;
}

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

    std::snprintf(cmd, sizeof cmd, "ip link set %s mtu %d qlen %d up",
                  o.iface.c_str(), o.mtu, o.tx_qlen);
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
    std::atomic<uint64_t> recoveries{0};
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
    // Frames too large for the framing layer, dropped rather than truncated.
    std::atomic<uint64_t> tx_oversize{0};
    std::atomic<uint64_t> loop_suppressed{0};
    std::atomic<uint64_t> tx_blocks{0}, tx_data_blocks{0}, tx_dma_bytes{0};
    std::atomic<uint64_t> tx_padding{0}, tx_full_flush{0}, tx_timeout_flush{0};
    std::atomic<uint64_t> rx_rejected{0}, rx_tun_err{0};
};
Stats g_stats;
std::atomic<bool> g_run{true};
} // namespace

// ── Transmit: TUN -> Framer -> fabric modulator ───────────────────────────
static sdr::LoopGuard g_loop_guard;
static std::mutex     g_loop_mx;

static void txLoop(int tun_fd, int tx_fd, DirectIioTx* direct_tx, const Opts& o) {
    Framer framer;
    const size_t tx_block = static_cast<size_t>(o.tx_block ? o.tx_block : o.pkt);
    DmaBlockAggregator agg(tx_block);
    uint32_t seq = 0;
    // Room for a whole Ethernet frame in raw mode, so an oversize frame is seen
    // and counted rather than silently arriving pre-truncated by the read.
    std::vector<uint8_t> pkt(o.raw_eth ? 2048u : static_cast<size_t>(o.mtu));
    bool burst_idle = true;

    auto encode = [&](const uint8_t* p, size_t n, uint8_t flags) {
        return
            // BW_5 is the nearest code to the 4 MHz RF bandwidth the bring-up
            // scripts set. The field is descriptive -- nothing in this path
            // acts on it -- but it should not claim a width the radio is not
            // using.
            framer.encode(p, n, flags, ModCode::QPSK, BwCode::BW_5,
                          o.node_id, seq++, nullptr, nullptr);
    };

    auto transmit = [&](std::vector<CompletedDmaBlock>& blocks) {
        for (auto& block : blocks) {
            size_t done = 0;
            auto w0 = std::chrono::steady_clock::now();
            if (direct_tx) {
                ssize_t w = direct_tx->push(block.bytes.data(), block.bytes.size());
                if (w < 0 || static_cast<size_t>(w) != block.bytes.size()) {
                    std::fprintf(stderr, "bridge: direct IIO TX push: %s (returned %zd)\n",
                                 w < 0 ? std::strerror(errno) : "short push", w);
                    g_stats.tx_err.fetch_add(1);
                    return false;
                }
                done = block.bytes.size();
            }
            while (done < block.bytes.size() && g_run.load()) {
                ssize_t w = ::write(tx_fd, block.bytes.data() + done,
                                    block.bytes.size() - done);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    g_stats.tx_err.fetch_add(1);
                    return false;
                }
                done += static_cast<size_t>(w);
            }
            g_stats.tx_stall_us_total.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - w0).count()));
            g_stats.tx_blocks.fetch_add(1);
            g_stats.tx_dma_bytes.fetch_add(block.bytes.size());
            g_stats.tx_padding.fetch_add(block.padding_bytes);
            if (block.data_packets) g_stats.tx_data_blocks.fetch_add(1);
        }
        blocks.clear();
        return true;
    };

    auto controlBlock = [&]() {
        std::vector<CompletedDmaBlock> blocks;
        static const uint8_t ka[16] = {0};
        while (blocks.empty()) {
            auto wire = encode(ka, sizeof ka, FL_CTRL);
            if (!agg.addFrame(wire, 0, false, blocks)) return false;
            g_stats.tx_idle.fetch_add(1);
        }
        return transmit(blocks);
    };

    while (g_run.load()) {
        struct pollfd pfd { tun_fd, POLLIN, 0 };
        // A complete block is paced by iio_writedev/the DAC. Waiting idle_ms
        // here as well creates a long RF-off gap after every block; measured
        // receivers decode the first control burst and then lose acquisition.
        // With keepalives enabled, poll without sleeping and immediately fill
        // the next block. The bounded child pipe limits avoidable queueing.
        int timeout = o.idle_ms > 0 ? 0 : 1000;
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
                if (!controlBlock()) break;
            }
            else {
                // The next packet starts a new RF burst and needs acquisition
                // training before its data, not after it.
                burst_idle = true;
            }
            continue;
        }

        if (o.idle_ms == 0 && burst_idle) {
            // A cold demodulator cannot decode a data frame placed at the very
            // start of a burst. Thirty-two control-only DMA blocks give its AGC,
            // timing and carrier loops time to settle. The peer drops these
            // frames, then receives the following data block. Since the local
            // transmitter becomes quiet afterwards, the peer can answer
            // without same-frequency self-interference.
            for (int i = 0; i < 32; ++i)
                if (!controlBlock()) return;
            burst_idle = false;
        }

        // Collect all packets arriving inside one short window. Each remains
        // an independently sequenced/CRC-protected RF frame, and complete
        // frames are packed into a DMA block without crossing its boundary.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds(o.batch_us);
        std::vector<CompletedDmaBlock> blocks;
        bool first = true;
        while (g_run.load()) {
            if (!first) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
                struct pollfd more { tun_fd, POLLIN, 0 };
                int wait_ms = static_cast<int>((left + 999) / 1000);
                int ready = ::poll(&more, 1, wait_ms);
                if (ready < 0) { if (errno == EINTR) continue; break; }
                if (ready == 0) break;
            }
            first = false;
            ssize_t n = ::read(tun_fd, pkt.data(), pkt.size());
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
            if (static_cast<size_t>(n) > MAX_PAYLOAD) {
                g_stats.tx_oversize.fetch_add(1);
                continue;
            }
            // L2 loop suppression. A frame whose SOURCE was last seen arriving
            // from the radio is our own echo returning over Ethernet, and must
            // not be sent back. Without this, two appliances on one switch
            // replicate without bound -- and that segment is the customer's
            // network, not ours. Only meaningful in raw-eth mode; a TUN carries
            // no Ethernet header to learn from.
            if (o.raw_eth) {
                bool fwd;
                {
                    std::lock_guard<std::mutex> lk(g_loop_mx);
                    fwd = g_loop_guard.shouldForwardToRadio(
                              pkt.data(), static_cast<size_t>(n), std::chrono::steady_clock::now());
                }
                if (!fwd) { g_stats.loop_suppressed.fetch_add(1); continue; }
            }
            auto wire = encode(pkt.data(), static_cast<size_t>(n), 0);
            const size_t before = blocks.size();
            if (!agg.addFrame(wire, static_cast<size_t>(n), true, blocks)) {
                g_stats.tx_oversize.fetch_add(1);
                continue;
            }
            if (blocks.size() != before) g_stats.tx_full_flush.fetch_add(1);
            g_stats.tx_pkts.fetch_add(1);
            g_stats.tx_bytes.fetch_add(static_cast<uint64_t>(n));
            if (!transmit(blocks)) return;
        }
        agg.flush(blocks);
        if (!blocks.empty()) g_stats.tx_timeout_flush.fetch_add(1);
        if (!transmit(blocks)) return;
    }
}

// ── Receive: fabric demodulator -> Deframer -> TUN ────────────────────────
//
// The four-offset decode lives in sdr/framing/OffsetDeframer.hpp so that the
// bridge and its test exercise the same code rather than two copies of it.
static void rxLoop(int tun_fd, int rx_fd, DirectIioRx* direct_rx, const Opts& o) {
    std::vector<uint8_t> buf(static_cast<size_t>(o.pkt));
    OffsetDeframer deframer;
    uint64_t crc_seen = 0, dup_seen = 0;
    bool pkt_checked = false;

    auto last_read = std::chrono::steady_clock::now();
    while (g_run.load()) {
        // A PIPE DOES NOT PRESERVE PACKET BOUNDARIES. iio_readdev writes one DMA
        // buffer at a time, but the pipe delivers whatever happens to be
        // available, so a single read returns a fragment. Decoding is scoped to
        // one transfer -- the byte grid is continuous only within it -- so a
        // fragment decoded as if it were a packet loses frames at both ends and
        // reports them as loss. Reassemble a full packet before decoding.
        ssize_t n = direct_rx ? direct_rx->refill(buf.data(), buf.size())
                              : readExact(rx_fd, buf.data(), buf.size());
        if (n <= 0) {
            if (n < 0) std::perror("read rx");
            if (!g_run.load()) break;
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
        g_stats.rx_crcerr.fetch_add(c - crc_seen);
        g_stats.rx_rejected.fetch_add(c - crc_seen);
        crc_seen = c;
        g_stats.rx_dup.fetch_add(d - dup_seen);    dup_seen = d;
        for (int i = 0; i < 4; ++i) g_stats.off_hits[i].store(deframer.offsetHits(i));

        for (auto& f : frames) {
            g_stats.rx_frames.fetch_add(1);
            // With coupled radios a unit can hear its own transmitter. Never
            // inject that packet back into its TUN, and do not let it obscure
            // a peer frame that happens to use the same sequence number.
            if (f.node_id == o.node_id) {
                g_stats.rx_self.fetch_add(1);
                continue;
            }
            // A keepalive is not user data; handing an empty or filler packet
            // to the kernel would be a bug visible only as junk on the wire.
            if (f.flags & FL_CTRL) { g_stats.rx_ctrl.fetch_add(1); continue; }
            if (f.payload.empty()) continue;

            // Learn the source BEFORE injecting: the echo can come back around
            // the switch faster than the next statistics tick.
            if (o.raw_eth) {
                std::lock_guard<std::mutex> lk(g_loop_mx);
                g_loop_guard.learnFromRadio(f.payload.data(), f.payload.size(),
                                            std::chrono::steady_clock::now());
            }
            ssize_t w = ::write(tun_fd, f.payload.data(), f.payload.size());
            if (w == static_cast<ssize_t>(f.payload.size()))
                g_stats.rx_bytes.fetch_add(static_cast<uint64_t>(w));
            else
                g_stats.rx_tun_err.fetch_add(1);
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
        else if (a == "--tx")    { o.tx_dev = next("--tx"); o.dev_explicit = true; }
        else if (a == "--rx")    { o.rx_dev = next("--rx"); o.dev_explicit = true; }
        else if (a == "--direct-iio-tx") o.direct_iio_tx = true;
        else if (a == "--direct-iio-rx") o.direct_iio_rx = true;
        else if (a == "--node-id") o.node_id = static_cast<uint32_t>(std::strtoul(next("--node-id").c_str(), nullptr, 0));
        else if (a == "--mtu")     o.mtu     = std::atoi(next("--mtu").c_str());
        else if (a == "--pkt")     o.pkt     = std::atoi(next("--pkt").c_str());
        else if (a == "--tx-block") o.tx_block = std::atoi(next("--tx-block").c_str());
        else if (a == "--tx-qlen") o.tx_qlen = std::atoi(next("--tx-qlen").c_str());
        else if (a == "--idle-ms") o.idle_ms = std::atoi(next("--idle-ms").c_str());
        else if (a == "--batch-us") o.batch_us = std::atoi(next("--batch-us").c_str());
        else if (a == "--route")   o.route   = next("--route");
        // --raw-eth names the NIC and selects the AF_PACKET backend. The option
        // was documented in usage() and honoured everywhere below, but never
        // parsed, so it was rejected as unknown and the backend was
        // unreachable from the command line.
        else if (a == "--raw-eth") { o.iface = next("--raw-eth"); o.raw_eth = true; }
        else if (a == "--forward") o.forward = true;
        else if (a == "--stats")   o.stats_s = std::atoi(next("--stats").c_str());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (!o.raw_eth && (o.local.empty() || o.peer.empty())) { usage(); return 2; }
    if (o.mtu > static_cast<int>(MAX_PAYLOAD)) {
        std::fprintf(stderr, "bridge: mtu %d exceeds MAX_PAYLOAD %zu; a packet that "
                             "large cannot be framed and would be dropped silently\n",
                     o.mtu, MAX_PAYLOAD);
        return 2;
    }
    if (o.batch_us < 0 || o.batch_us > 1000000) {
        std::fprintf(stderr, "bridge: --batch-us must be between 0 and 1000000\n");
        return 2;
    }
    if (o.tx_block && (!o.direct_iio_tx || o.tx_block < 2048 || (o.tx_block % 4))) {
        std::fprintf(stderr, "bridge: --tx-block requires --direct-iio-tx and a multiple of 4 >= 2048\n");
        return 2;
    }
    if (o.tx_qlen < 1 || o.tx_qlen > 10000) {
        std::fprintf(stderr, "bridge: --tx-qlen must be between 1 and 10000\n");
        return 2;
    }
    if (o.dev_explicit && (o.direct_iio_tx || o.direct_iio_rx)) {
        std::fprintf(stderr, "bridge: direct IIO options cannot be combined with --tx/--rx\n");
        return 2;
    }
    if (o.pkt < 2048 || (o.pkt % 4)) {
        std::fprintf(stderr, "bridge: --pkt must be a multiple of 4 and at least 2048\n");
        return 2;
    }
    if (!o.dev_explicit && !verifyFpgaPacketBytes(o.pkt)) return 1;

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

    // A dead child must not take the bridge down with it.
    ::signal(SIGPIPE, SIG_IGN);

    int tx_fd = -1, rx_fd = -1;
    std::unique_ptr<DirectIioTx> direct_tx;
    std::unique_ptr<DirectIioRx> direct_rx;
    if (o.dev_explicit) {
        // Raw paths: the test harness's FIFOs, or a deliberate override. This
        // does NOT work against a real radio on 6.12 -- see spawnIio above.
        tx_fd = ::open(o.tx_dev.c_str(), O_WRONLY);
        if (tx_fd < 0) {
            std::fprintf(stderr, "bridge: open %s: %s\n", o.tx_dev.c_str(), std::strerror(errno));
            return 1;
        }
        rx_fd = ::open(o.rx_dev.c_str(), O_RDONLY);
        if (rx_fd < 0) {
            std::fprintf(stderr, "bridge: open %s: %s\n", o.rx_dev.c_str(), std::strerror(errno));
            ::close(tx_fd); return 1;
        }
        std::fprintf(stderr, "bridge: raw device paths (no libiio) -- test mode\n");
    } else {

    // Buffer size in SAMPLES: 4 bytes per sample with both channels enabled, so
    // one buffer is exactly one DMA packet. The receive path decodes per packet,
    // and matching the two keeps that scoping meaningful across the pipe.
    char nbuf[32];
    std::snprintf(nbuf, sizeof nbuf, "%d", o.pkt / 4);
    const int tx_bytes = o.tx_block ? o.tx_block : o.pkt;
    char tx_nbuf[32];
    std::snprintf(tx_nbuf, sizeof tx_nbuf, "%d", tx_bytes / 4);

    if (o.direct_iio_tx) {
        direct_tx.reset(new DirectIioTx);
        if (!direct_tx->open(static_cast<size_t>(tx_bytes / 4),
                             static_cast<size_t>(tx_bytes))) return 1;
    } else {
        const char* tx_argv[] = { "iio_writedev", "-b", nbuf,
                                  "cf-ad9361-dds-core-lpc", "voltage0", "voltage1", nullptr };
        tx_fd = spawnIio(tx_argv, true, &g_tx_pid);
        if (tx_fd < 0) { std::fprintf(stderr, "bridge: could not start iio_writedev\n"); return 1; }
    }

    // Give the writer time to open the buffer, then redo the source select it
    // just undid. Do not open RX yet. On this image, enabling both IIO buffers
    // before the first TX transfer leaves the TX DMAC idle: its IRQ count does
    // not advance and the fabric TX probe is all zero. Starting TX first and
    // adding RX after a block is in flight was measured at 256/256 non-zero TX
    // samples with both DMA IRQ counters advancing.
    ::usleep(3000 * 1000);
    selectDmaSource();
    if (direct_tx)
        std::fprintf(stderr, "bridge: direct single-buffer libiio TX, buffer=%s samples/%d bytes (RX deferred)\n",
                     tx_nbuf, tx_bytes);
    else
        std::fprintf(stderr, "bridge: iio_writedev pid=%d buffer=%s samples (RX deferred)\n",
                     (int)g_tx_pid, nbuf);
    }

    int tun_fd = o.raw_eth ? packetOpen(o.iface, o.mtu) : tunOpen(o.iface, o.mtu);
    if (tun_fd < 0) { ::close(tx_fd); ::close(rx_fd); return 1; }
    // Only the TUN path needs addressing; an AF_PACKET bridge deliberately has
    // no IP of its own.
    if (!o.raw_eth && !tunConfigure(o)) { ::close(tun_fd); ::close(tx_fd); ::close(rx_fd); return 1; }

    std::thread tx(txLoop, tun_fd, tx_fd, direct_tx.get(), std::cref(o));

    if (!o.dev_explicit) {
        // txLoop immediately fills and submits one complete DMA block. Allow
        // that transfer to start before enabling RX; see the ordering note
        // above. This is initialization only, not steady-state pacing.
        ::usleep(1000 * 1000);
        char nbuf[32];
        std::snprintf(nbuf, sizeof nbuf, "%d", o.pkt / 4);
        if (o.direct_iio_rx) {
            direct_rx.reset(new DirectIioRx);
            if (!direct_rx->open(static_cast<size_t>(o.pkt / 4),
                                 static_cast<size_t>(o.pkt))) {
                g_run.store(false);
                tx.join();
                ::close(tun_fd); ::close(tx_fd);
                return 1;
            }
            std::fprintf(stderr, "bridge: deferred direct single-buffer libiio RX, buffer=%s samples\n", nbuf);
        } else {
            const char* rx_argv[] = { "iio_readdev", "-b", nbuf,
                                      "cf-ad9361-lpc", "voltage0", "voltage1", nullptr };
            rx_fd = spawnIio(rx_argv, false, &g_rx_pid);
            if (rx_fd < 0) {
                std::fprintf(stderr, "bridge: could not start deferred iio_readdev\n");
                g_run.store(false);
                tx.join();
                ::close(tun_fd); ::close(tx_fd);
                return 1;
            }
            std::fprintf(stderr, "bridge: deferred iio_readdev pid=%d\n", (int)g_rx_pid);
        }
    }
    std::thread rx(rxLoop, tun_fd, rx_fd, direct_rx.get(), std::cref(o));

    auto t_start = std::chrono::steady_clock::now();
    uint64_t last_decode = 0;
    bool     warned_short = false;
    uint64_t last_short   = 0;
    uint64_t last_rx_dma  = g_stats.rx_dma.load();
    uint64_t last_frames  = g_stats.rx_frames.load();
    DemodStatus last_demod = demodStatus();
    unsigned stalled_intervals = 0;
    // Recovery is armed only after this process has decoded a real/control
    // frame.  With no RF at power-up, noise may exercise the timing loop and
    // increase mu_clamped; that alone must not cause periodic resets.
    bool recovery_armed = false;
    // Baseline the kernel's interface drop counters.
    //
    // They are cumulative since boot and count drops from ANY source, so
    // reporting them raw attributes the board's own pre-bridge history to the
    // bridge. Both units show it: 4 and 3 dropped against 8 and 3 transmitted,
    // all of it the local stack emitting before the interface was ready, and
    // static ever since. As a delta that reads 0, which is the truth about what
    // the bridge dropped.
    const uint64_t base_tx_drop = ifCounter(o.iface, "tx_dropped");
    const uint64_t base_rx_drop = ifCounter(o.iface, "rx_dropped");
    while (g_run.load() && o.stats_s > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(o.stats_s));
        {
            uint64_t t = ifCounter(o.iface, "tx_dropped");
            uint64_t r = ifCounter(o.iface, "rx_dropped");
            g_stats.tun_tx_drop.store(t > base_tx_drop ? t - base_tx_drop : 0);
            g_stats.tun_rx_drop.store(r > base_rx_drop ? r - base_rx_drop : 0);
        }
        std::fprintf(stderr,
            "bridge: tx %llu pkts / %llu B (idle %llu, err %llu) | "
            "rx %llu dma, %llu frames, %llu B (crcerr %llu, dup %llu, ctrl %llu) | "
            "offsets %llu/%llu/%llu/%llu | recoveries %llu\n",
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
            (unsigned long long)g_stats.off_hits[3].load(),
            (unsigned long long)g_stats.recoveries.load());

        const uint64_t blocks = g_stats.tx_blocks.load();
        const uint64_t data_blocks = g_stats.tx_data_blocks.load();
        const uint64_t dma_bytes = g_stats.tx_dma_bytes.load();
        const uint64_t payload_bytes = g_stats.tx_bytes.load();
        const double util = dma_bytes ? 100.0 * payload_bytes / dma_bytes : 0.0;
        const double ppb = data_blocks ? 1.0 * g_stats.tx_pkts.load() / data_blocks : 0.0;
        std::fprintf(stderr,
            "bridge: aggregate tx blocks %llu (data %llu), dma %llu B, padding %llu B, "
            "util %.2f%%, %.2f pkt/data-block, full %llu, timeout %llu | "
            "rx rejected %llu, tun-write-err %llu\n",
            (unsigned long long)blocks,
            (unsigned long long)data_blocks,
            (unsigned long long)dma_bytes,
            (unsigned long long)g_stats.tx_padding.load(), util, ppb,
            (unsigned long long)g_stats.tx_full_flush.load(),
            (unsigned long long)g_stats.tx_timeout_flush.load(),
            (unsigned long long)g_stats.rx_rejected.load(),
            (unsigned long long)g_stats.rx_tun_err.load());

        // ── Autonomous demodulator recovery ─────────────────────────────
        // A stalled core was measured with RX DMA still advancing, no frames
        // decoding, and mu_clamped rapidly increasing.  Require that exact
        // combination for two reporting intervals.  One recovery attempt then
        // disarms the monitor until a frame is decoded again, preventing an
        // absent RF signal from producing a reset loop.
        const uint64_t dma_now = g_stats.rx_dma.load();
        const uint64_t frames_now = g_stats.rx_frames.load();
        const uint64_t dma_delta = dma_now - last_rx_dma;
        const uint64_t frame_delta = frames_now - last_frames;
        DemodStatus demod_now = demodStatus();
        if (frame_delta > 0) {
            recovery_armed = true;
            stalled_intervals = 0;
        } else if (recovery_armed && demod_now.valid && last_demod.valid) {
            const uint32_t mu_delta = demod_now.mu_clamped - last_demod.mu_clamped;
            if (dma_delta >= 2 && mu_delta >= 32) ++stalled_intervals;
            else stalled_intervals = 0;
            if (stalled_intervals >= 2) {
                if (demodSoftResetDrained()) {
                    g_stats.recoveries.fetch_add(1);
                    std::fprintf(stderr,
                        "bridge: RECOVERY drained demod soft_reset "
                        "(dma +%llu, frames +0, mu_clamped +%u, lock=%u)\n",
                        (unsigned long long)dma_delta, mu_delta,
                        demod_now.lock_count);
                } else {
                    std::fprintf(stderr,
                        "bridge: WARNING recovery needed but /dev/mem reset failed\n");
                }
                recovery_armed = false;
                stalled_intervals = 0;
            }
        } else {
            stalled_intervals = 0;
        }
        last_rx_dma = dma_now;
        last_frames = frames_now;
        last_demod = demod_now;

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
            "%s drops tx %llu rx %llu (since start) | oversize %llu | loopsup %llu\n",
            busy_win, busy,
            (unsigned long long)g_stats.decode_us_max.load(),
            (unsigned long long)g_stats.rx_gap_us_max.load(),
            (unsigned long long)g_stats.rx_short.load(),
            (unsigned long long)(g_stats.tx_stall_us_total.load() / 1000),
            o.iface.c_str(),
            (unsigned long long)g_stats.tun_tx_drop.load(),
            (unsigned long long)g_stats.tun_rx_drop.load(),
            (unsigned long long)g_stats.tx_oversize.load(),
            (unsigned long long)g_stats.loop_suppressed.load());
        if (busy_win > 70.0)
            std::fprintf(stderr,
                "bridge: WARNING decode is using %.0f%% of one core. Loss from here\n"
                "        is a CPU limit, NOT the radio -- do not chase it as RF.\n",
                busy_win);
        // Warn only if short reads are STILL ACCUMULATING. A burst of them at
        // startup is normal and harmless: the helper's pipe carries partial
        // data while the stream establishes, and the count then stops dead --
        // measured frozen at 60 while rx_dma climbed from 546 to 598. Warning
        // on that would send an operator after a fault that had already
        // stopped, which is the opposite of what these counters are for.
        uint64_t short_now = g_stats.rx_short.load();
        bool still_growing = short_now > last_short;
        last_short = short_now;
        if (still_growing && g_stats.rx_dma.load() > 32 && !warned_short) {
            warned_short = true;
            std::fprintf(stderr,
                "bridge: WARNING short reads are ONGOING (%llu so far, not a full\n"
                "        %d-byte packet); the byte\n"
                "        grid is only continuous within a transfer, so frames are\n"
                "        being lost at boundaries. Check --pkt against PKT_BYTES.\n",
                (unsigned long long)g_stats.rx_short.load(), o.pkt);
        }
    }
    if (o.stats_s <= 0) { tx.join(); rx.join(); }
    else { tx.detach(); rx.detach(); }
    return 0;
}
