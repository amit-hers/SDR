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
#include "sdr/bridge/TrafficClass.hpp"
#include "sdr/bridge/HealthProbe.hpp"
#include "sdr/bridge/PeerHandshake.hpp"
#include "sdr/framing/PacketReader.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/framing/DmaBlockAggregator.hpp"

#include <algorithm>
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
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <csignal>
#include <pthread.h>
#include <sched.h>
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
    // Traffic classes. bulk_min is the size above which a frame is treated as
    // bulk when it carries no 802.1Q priority; the budgets bound queue latency.
    std::size_t bulk_min       = 512;
    std::size_t q_ctrl_frames  = 256;
    std::size_t q_bulk_frames  = 256;
    unsigned    bulk_every     = 8;
    uint64_t    link_bps       = 6960000;   // measured payload goodput
    unsigned    ctrl_budget_ms = 10;
    unsigned    bulk_budget_ms = 100;
    // The largest FRAME the radio carries (== MAX_PAYLOAD). In TUN mode that is
    // the interface MTU directly; in raw-eth mode the interface MTU is this
    // minus the 14-byte Ethernet header, i.e. a standard 1500.
    int         mtu     = static_cast<int>(MAX_PAYLOAD);
    int         tx_kbufs = 2;          // direct-IIO TX kernel buffers in flight
    int         tx_rt_prio = 0;        // SCHED_FIFO priority for the TX thread, 0 = off
    int         tx_pipe_blocks = 4;    // iio_writedev pipe depth, in DMA blocks
    int         pkt     = 32768;       // PKT_BYTES in axis_packetizer.v
    int         tx_block = 0;          // direct-IIO TX bytes; 0 uses pkt
    int         tx_qlen = 32;          // bound bulk backlog without dropping interactive traffic
    int         idle_ms = 200;         // keepalive cadence when there is no traffic
    int         batch_us = 3000;       // collect TUN packets before a data-block flush
    bool        forward = false;
    std::string route;                 // network behind the peer, via the radio
    int         stats_s = 5;
    // Active health probe: an echoed request through the real RF path, timed
    // on the sender's own clock alone (see HealthProbe.hpp -- no cross-unit
    // clock sync needed for an RTT). Same cadence family as --stats; 0
    // disables it. On by default: it only ever substitutes for an otherwise
    // identical-size HELLO filler frame that idle airtime is already
    // spending, so there is no meaningful new airtime cost to turning it on.
    int         probe_interval_s = 5;
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
      "  --mtu N          largest frame carried (default 1514 == MAX_PAYLOAD);\n"
      "                   raw-eth sets the link MTU to N-14, TUN to N\n"
      "  --tx-kbufs N     direct-IIO TX kernel buffers (default 2). Each is one\n"
      "                   block of airtime queued ahead of a new packet\n"
      "  --tx-pipe-blocks N  iio_writedev pipe depth in DMA blocks (default 4).\n"
      "                   Fewer = lower latency, less margin against a TX stall\n"
      "  --tx-rt N        run the TX thread at SCHED_FIFO priority N (1..90, 0=off).\n"
      "                   Required with a shallow TX queue: a late block starves\n"
      "                   the modulator, which emits ZERO samples, and the peer\n"
      "                   loses carrier/timing lock (not just that block's data)\n"
      "  --pkt N          DMA packet size, must equal PKT_BYTES (default 32768)\n"
      "  --tx-block N     direct-IIO TX block bytes (default: same as --pkt)\n"
      "  --tx-qlen N      TUN transmit queue length in packets (default 32)\n"
      "  --idle-ms N      keepalive cadence, 0 disables (default 200)\n"
      "  --batch-us N     TX aggregation window in microseconds (default 3000)\n"
      "  --route CIDR     a network behind the peer, routed over the radio\n"
      "  --forward        enable IPv4 forwarding (eth0 <-> radio)\n"
      "  --stats N        statistics interval in seconds, 0 disables\n"
      "  --probe-interval-s N  active end-to-end health probe cadence in\n"
      "                   seconds, 0 disables (default 5). An echoed request\n"
      "                   through the real RF path, timed on this unit's own\n"
      "                   clock alone; reported as rtt_us p50/p95/p99/max and\n"
      "                   a delivery ratio in bridge_stats.json\n");
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
static int spawnIio(const char* const argv[], bool to_child, pid_t* pid_out,
                    int dma_block_bytes) {
    int fds[2];
    if (::pipe(fds) < 0) { std::perror("pipe"); return -1; }
    // Do not let continuous keepalive blocks build a deep queue in front of a
    // newly arrived data packet: the link transmits idle fill continuously, so
    // every byte of pipe is always full and a new packet waits behind all of
    // it. The pipe is sized in DMA blocks (--tx-pipe-blocks), whatever the
    // block size is; it was a hard-coded 32768 from when blocks were 32 KiB.
    // At 8192-byte blocks each block of pipe is ~8.8 ms of queue per direction
    // (measured: 160 ms ping RTT at four blocks). iio_writedev adds its own
    // four kernel buffers behind this and offers no option to shrink them.
    // The depth is also the margin against a stall in this process: if the
    // pipe drains, the DAC underruns and the far demodulator loses lock.
    // RX is unaffected: its pipe must retain the kernel default so a brief
    // decode delay does not discard data.
#ifdef F_SETPIPE_SZ
    if (to_child) {
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
    using SetKernelBuffers = int (*)(const iio_device*, unsigned);

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

    bool open(size_t samples, size_t expected_bytes, int kernel_buffers) {
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
        // Queue depth IS latency here. The link transmits continuously (idle
        // fill), so every kernel buffer is always full and a newly arrived
        // packet waits behind all of them: at 8192 B/block and ~8.8 ms of air
        // per block, libiio's default of 4 was ~35 ms per direction before the
        // packet even reached the modulator. Two is the minimum that keeps the
        // DAC fed while one is being filled. Optional symbol: an older libiio
        // without it keeps its default rather than failing to start.
        auto set_kernel_buffers = reinterpret_cast<SetKernelBuffers>(
            ::dlsym(so_, "iio_device_set_kernel_buffers_count"));
        if (set_kernel_buffers) {
            if (set_kernel_buffers(dev, static_cast<unsigned>(kernel_buffers)) != 0)
                std::fprintf(stderr, "bridge: WARNING could not set %d TX kernel buffers; using libiio default\n",
                             kernel_buffers);
        } else {
            std::fprintf(stderr, "bridge: WARNING libiio lacks iio_device_set_kernel_buffers_count; TX queue depth is the default\n");
        }
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
// The capture interface's own MAC. Frames addressed TO it are for this unit's
// stack (its SSH, its ARP) and never cross the radio; frames arriving from the
// radio that carry it, as source or destination, are echoes and are never
// injected. Measured on one switch without this: a PC's ARP to unit B's own
// address looped B -> RF -> A -> switch -> B at 1,800 frames/s, because the
// source (the PC) is legitimately local and the copy returns on the WIRE,
// where neither the source rule nor the content rule can see it.
static uint8_t g_own_mac[6] = {0, 0, 0, 0, 0, 0};
static bool    g_own_mac_ok = false;
static std::atomic<uint64_t> g_to_self{0}, g_from_self{0};

static int packetOpen(const std::string& iface, int frame_max) {
    // The link MTU counts IP bytes; the frame the radio carries adds the
    // 14-byte Ethernet header. Setting the MTU to the frame limit itself (as
    // this once did, 1400) told attached hosts they could send 1414-byte
    // frames, which were then dropped as oversize -- TCP stalled while ping
    // worked. The MTU an attached host sees must be what actually fits.
    const int mtu = frame_max - 14;
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
    {
        struct ifreq hw;
        std::memset(&hw, 0, sizeof hw);
        std::strncpy(hw.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFHWADDR, &hw) == 0) {
            std::memcpy(g_own_mac, hw.ifr_hwaddr.sa_data, 6);
            g_own_mac_ok = true;
        } else {
            std::fprintf(stderr, "bridge: WARNING cannot read %s MAC: %s; frames to this unit will cross the radio\n",
                         iface.c_str(), std::strerror(errno));
        }
    }

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

    // Turn OFF generic receive offload on the capture interface. GRO merges
    // consecutive TCP (and on 6.12, UDP) segments into one large skb BEFORE
    // packet taps see it, so this socket received "frames" of several KB from
    // hosts that never sent anything over 1514 bytes. They were counted
    // oversize and dropped, silently, and only TCP was affected: ping worked,
    // SSH hung at the key exchange, TCP throughput collapsed to 52 kbit/s.
    // The board has no ethtool, so the ioctl is issued here, every start.
    {
        struct ifreq gr;
        std::memset(&gr, 0, sizeof gr);
        std::strncpy(gr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        struct ethtool_value ev;
        ev.cmd = ETHTOOL_SGRO; ev.data = 0;
        gr.ifr_data = reinterpret_cast<char*>(&ev);
        int cs = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (cs < 0 || ::ioctl(cs, SIOCETHTOOL, &gr) < 0)
            std::fprintf(stderr, "bridge: WARNING could not disable GRO on %s: %s\n"
                                 "bridge:         coalesced TCP frames will be dropped as oversize\n",
                         iface.c_str(), std::strerror(errno));
        else
            std::fprintf(stderr, "bridge: GRO disabled on %s\n", iface.c_str());
        if (cs >= 0) ::close(cs);
    }

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
                     "bridge: WARNING %s mtu is %d but the radio carries %d-byte frames (mtu %d);\n"
                     "bridge:         larger frames will be counted oversize and dropped\n",
                     iface.c_str(), actual_mtu, frame_max, mtu);
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
    std::atomic<uint64_t> loop_suppressed{0}, loop_local_copies{0};
    // Frames that existed only across an RX DMA packet boundary. Before the
    // boundary window these were lost with NO counter moving at all, which
    // is why full-size frames failed silently and episodically.
    std::atomic<uint64_t> rx_boundary{0};
    std::atomic<uint64_t> rx_inject_err{0};
    std::atomic<uint64_t> q_ctrl_depth{0}, q_bulk_depth{0};
    std::atomic<uint64_t> q_ctrl_drop{0},  q_bulk_drop{0};
    std::atomic<uint64_t> q_drain_ms{0};
    // Peer handshake state. Guarded by its own mutex: written by the receive
    // thread when a HELLO arrives, read by the transmit thread and the stats
    // printer.
    std::atomic<int>      peer_verdict{0};   // 0 unknown, 1 compatible, 2 incompatible
    std::atomic<uint64_t> peer_hellos{0};
    // When the last HELLO arrived. A verdict with no recent evidence is STALE,
    // not still true: if the peer's frequency crossing breaks, this unit stops
    // hearing it entirely and would otherwise keep reporting the last good
    // verdict for ever. Measured: UNIT-A reported COMPATIBLE indefinitely after
    // UNIT-B was deliberately misconfigured, because no contradicting HELLO can
    // arrive when nothing can be heard at all.
    std::atomic<uint64_t> peer_last_ms{0};
    std::atomic<uint64_t> tx_blocks{0}, tx_data_blocks{0}, tx_dma_bytes{0};
    std::atomic<uint64_t> tx_padding{0}, tx_full_flush{0}, tx_timeout_flush{0};
    std::atomic<uint64_t> rx_rejected{0}, rx_tun_err{0};

    // Active health probe (HealthProbe.hpp). RTT samples themselves are not
    // atomics -- a rolling window needs a container -- so they live in
    // g_probe_rtt_us below, guarded by g_probe_mx; these three counts are
    // pure reporting and fit the same lock-free pattern as everything else
    // here.
    std::atomic<uint64_t> probe_sent{0}, probe_delivered{0}, probe_lost{0};

    // Zero the REPORTING counters an operator means by "clear counters" --
    // deliberately NOT every field, and NOT a single whole-struct reset.
    //
    // rx_dma, rx_frames, rx_dup, rx_crcerr and recoveries are excluded because
    // the stats thread's own recovery detector reads them back to compute a
    // DELTA against local (non-atomic) baselines it keeps between ticks
    // (last_rx_dma, last_frames). Zeroing the atomic without also resetting
    // those locals is a 64-bit UNDERFLOW on the very next tick (0 - a large
    // prior value wraps to a huge positive number), which reads as a runaway
    // DMA advance and can trigger a demodulator reset THIS ACTION never asked
    // for. Reaching into the stats thread's local variables from here would
    // need a lock this hot path does not otherwise want. Simplest safe fix:
    // never clear what feeds a decision, only what merely gets reported.
    //
    // peer_verdict/peer_hellos/peer_last_ms are excluded for a different
    // reason -- they are CURRENT STATE (what the peer is doing right now),
    // not an accumulating count, and "clearing" current state to UNKNOWN would
    // misrepresent a link that never stopped being compatible. It repopulates
    // within one HELLO interval regardless, so excluding it costs nothing.
    //
    // q_ctrl_depth/q_bulk_depth are excluded for the same reason: a live
    // gauge, not a counter.
    void clearReportingCounters() {
        tx_pkts = 0; tx_bytes = 0; tx_idle = 0; tx_err = 0;
        rx_bytes = 0; rx_ctrl = 0; rx_self = 0;
        for (auto& h : off_hits) h = 0;
        decode_us_total = 0; decode_us_max = 0;
        rx_gap_us_max = 0; tx_stall_us_total = 0; rx_short = 0;
        // tun_tx_drop/tun_rx_drop deliberately NOT here: the stats loop
        // recomputes both, every tick, as a delta against a fixed baseline
        // captured at process start -- clearing them would be undone by that
        // same tick's own recomputation, before anyone could observe it.
        tx_oversize = 0;
        loop_suppressed = 0; loop_local_copies = 0;
        rx_boundary = 0; rx_inject_err = 0;
        q_ctrl_drop = 0; q_bulk_drop = 0; q_drain_ms = 0;
        tx_blocks = 0; tx_data_blocks = 0; tx_dma_bytes = 0;
        tx_padding = 0; tx_full_flush = 0; tx_timeout_flush = 0;
        rx_rejected = 0; rx_tun_err = 0;
        // probe_sent/delivered/lost deliberately ARE cleared here (unlike
        // rx_dma/rx_frames/recoveries above): nothing derives a decision
        // from their absolute value, only from RTT samples riding alongside
        // them in g_probe_rtt_us, which this method does not touch.
        probe_sent = 0; probe_delivered = 0; probe_lost = 0;
    }
};
Stats g_stats;
// Set only from the SIGUSR1 handler (a single atomic store is the entire
// handler body -- async-signal-safe); acted on from ordinary code in the
// stats thread's own loop, never from signal context, since
// clearReportingCounters() and the fprintf below it are not signal-safe.
std::atomic<bool> g_clear_counters_requested{false};
std::atomic<bool> g_run{true};
} // namespace

// ── Transmit: TUN -> Framer -> fabric modulator ───────────────────────────
// Small readers for the identity above. Each returns 0 on failure rather than
// guessing, so an unreadable value is advertised as 0 and compared as unequal
// instead of silently matching whatever the peer has.
static uint64_t readSysfsU64(const char* path) {
    FILE* f = std::fopen(path, "r"); if (!f) return 0;
    unsigned long long v = 0; if (std::fscanf(f, "%llu", &v) != 1) v = 0;
    std::fclose(f); return v;
}
static uint32_t readReg32(unsigned long addr) {
    int fd = ::open("/dev/mem", O_RDONLY | O_SYNC); if (fd < 0) return 0;
    const long ps = sysconf(_SC_PAGESIZE);
    off_t base = static_cast<off_t>(addr & ~(unsigned long)(ps - 1));
    void* m = ::mmap(nullptr, ps, PROT_READ, MAP_SHARED, fd, base);
    uint32_t v = 0;
    if (m != MAP_FAILED) {
        v = *reinterpret_cast<volatile uint32_t*>(
                static_cast<char*>(m) + (addr - static_cast<unsigned long>(base)));
        ::munmap(m, ps);
    }
    ::close(fd); return v;
}
static void readIfaceMac(const std::string& iface, uint8_t out[6]) {
    std::memset(out, 0, 6);
    std::string p = "/sys/class/net/" + iface + "/address";
    FILE* f = std::fopen(p.c_str(), "r"); if (!f) return;
    unsigned a,b,c,d,e,g2;
    if (std::fscanf(f, "%x:%x:%x:%x:%x:%x", &a,&b,&c,&d,&e,&g2) == 6) {
        out[0]=a; out[1]=b; out[2]=c; out[3]=d; out[4]=e; out[5]=g2;
    }
    std::fclose(f);
}

static sdr::LoopGuard g_loop_guard;
static sdr::PeerIdentity g_me;                 // filled at start-up
static std::string       g_peer_reasons;       // why the peer is incompatible
static std::mutex        g_peer_mx;
static std::mutex     g_loop_mx;

// ── Active health probe state ───────────────────────────────────────────
// One mutex covers all of it: probes are low-rate by design (a handful of
// seconds apart), so this is never on a hot path and a single lock keeps
// the bookkeeping easy to reason about, unlike the per-frame counters above
// which are lock-free atomics for exactly the opposite reason.
static std::mutex     g_probe_mx;
static uint32_t       g_probe_next_seq = 0;
static uint64_t       g_probe_last_sent_us = 0;
// Requests THIS unit sent and has not yet seen a reply for. A deque, not a
// map: outstanding count is always small (one in flight is the common
// case), so a linear scan to find a matching seq costs nothing measurable,
// and bounding it below is simpler than bounding a map.
struct OutstandingProbe { uint32_t seq; uint64_t send_time_us; };
static std::deque<OutstandingProbe> g_probe_outstanding;
static constexpr std::size_t MAX_OUTSTANDING_PROBES = 8;
// Timeout for an outstanding probe with no reply. Generous relative to the
// probe interval itself (never less than 3x it, floor 3s) so a single slow
// or lightly-delayed round trip is not misreported as loss.
static constexpr uint64_t PROBE_TIMEOUT_US = 5'000'000;
// Replies THIS unit owes, from requests it received. Bounded for the same
// reason as g_probe_outstanding -- a peer that somehow floods requests must
// not grow this without limit; a full queue just drops the oldest owed
// reply rather than blocking anything on the hot receive path.
static std::deque<sdr::ProbeMessage> g_probe_pending_replies;
static constexpr std::size_t MAX_PENDING_REPLIES = 8;
// Rolling window of recent successful RTTs, microseconds. Bounded, not
// time-windowed: a fixed sample count is simpler and, at one probe every
// few seconds, still covers a long enough span to be meaningful (200
// samples at the default 5s interval is ~17 minutes).
static std::deque<uint64_t> g_probe_rtt_us;
static constexpr std::size_t MAX_PROBE_SAMPLES = 200;

static uint64_t nowMonotonicUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Drop outstanding requests older than PROBE_TIMEOUT_US and count each as
// lost. Called with g_probe_mx already held.
static void sweepStaleProbesLocked(uint64_t now_us) {
    while (!g_probe_outstanding.empty() &&
           now_us - g_probe_outstanding.front().send_time_us > PROBE_TIMEOUT_US) {
        g_probe_outstanding.pop_front();
        g_stats.probe_lost.fetch_add(1);
    }
}

// What the next idle control-filler frame should carry, in priority order:
// (1) a reply this unit owes -- sent promptly, ahead of routine keepalive
// traffic, because queueing it behind anything else directly inflates the
// ORIGINATOR's measured RTT by however long this side made it wait; (2) a
// newly originated request, at most once per --probe-interval-s; (3) the
// ordinary HELLO keepalive otherwise. When probing is disabled
// (probe_interval_s <= 0) this always falls through to (3), byte-identical
// to the bridge's behavior before this existed.
static std::vector<uint8_t> nextControlPayload(const Opts& o) {
    {
        std::lock_guard<std::mutex> lk(g_probe_mx);
        if (!g_probe_pending_replies.empty()) {
            sdr::ProbeMessage m = g_probe_pending_replies.front();
            g_probe_pending_replies.pop_front();
            return sdr::encodeProbe(sdr::PROBE_REPLY_MAGIC, m);
        }
    }
    if (o.probe_interval_s > 0) {
        const uint64_t interval_us = (uint64_t)o.probe_interval_s * 1'000'000;
        const uint64_t now = nowMonotonicUs();
        std::lock_guard<std::mutex> lk(g_probe_mx);
        if (now - g_probe_last_sent_us >= interval_us) {
            g_probe_last_sent_us = now;
            sweepStaleProbesLocked(now);
            sdr::ProbeMessage m;
            m.seq = g_probe_next_seq++;
            m.send_time_us = now;
            if (g_probe_outstanding.size() >= MAX_OUTSTANDING_PROBES) g_probe_outstanding.pop_front();
            g_probe_outstanding.push_back({m.seq, now});
            g_stats.probe_sent.fetch_add(1);
            return sdr::encodeProbe(sdr::PROBE_REQUEST_MAGIC, m);
        }
    }
    return sdr::encodeHello(g_me);
}

// A reply arrived: find the matching outstanding request, record its RTT,
// and count it delivered. A seq with no match is a duplicate or a reply
// that already timed out and was swept -- silently ignored, since it is
// neither new information nor an error.
static void onProbeReply(const sdr::ProbeMessage& reply) {
    const uint64_t now = nowMonotonicUs();
    std::lock_guard<std::mutex> lk(g_probe_mx);
    for (auto it = g_probe_outstanding.begin(); it != g_probe_outstanding.end(); ++it) {
        if (it->seq == reply.seq) {
            const uint64_t rtt_us = now - it->send_time_us;
            g_probe_outstanding.erase(it);
            g_stats.probe_delivered.fetch_add(1);
            if (g_probe_rtt_us.size() >= MAX_PROBE_SAMPLES) g_probe_rtt_us.pop_front();
            g_probe_rtt_us.push_back(rtt_us);
            return;
        }
    }
}

// Exact percentile off a small, bounded, already-in-memory sample set: a
// copy-and-sort costs nothing measurable at this scale (<= MAX_PROBE_SAMPLES
// elements, computed only once per --stats tick, never per-frame), so there
// is no reason to reach for a streaming approximation.
static uint64_t percentile(std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::size_t idx = (std::size_t)(p * (double)(sorted.size() - 1));
    return sorted[idx];
}

static void txLoop(int tun_fd, int tx_fd, DirectIioTx* direct_tx, const Opts& o) {
    // REAL-TIME PRIORITY, when asked for.
    //
    // The transmitter has a hard deadline the receiver does not: the DAC
    // consumes samples at a fixed rate, and if the next block is late the
    // modulator emits ZERO IQ samples (qpsk_mod.cpp: `if (s_axis_bits.empty())
    // goto rrc_out`). Zero is not a gap in the data, it is a gap in the
    // CARRIER, so the peer's AGC, timing and Costas loops all lose lock and
    // the outage lasts far longer than the missed block. Measured with a
    // shallow queue and no priority: 2048-byte blocks (2.1 ms each) gave
    // 25-32 ms RTT -- the target -- at 50% packet loss, because the decode
    // thread's CPU bursts (up to 8 ms per packet) made the TX thread late.
    //
    // Deep queues are the alternative and cost latency directly: the default
    // 8192-byte blocks, 4 in the pipe plus libiio's 4, are ~77 ms per
    // direction and measure 160 ms RTT. Priority buys the shallow queue
    // instead. The thread blocks on poll() and on the DAC write, so it yields
    // whenever it has nothing to do and cannot monopolise a core.
    if (o.tx_rt_prio > 0) {
        struct sched_param sp;
        std::memset(&sp, 0, sizeof sp);
        sp.sched_priority = o.tx_rt_prio;
        const int rc = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0)
            std::fprintf(stderr, "bridge: WARNING could not set TX thread SCHED_FIFO %d: %s\n"
                                 "bridge:         a shallow TX queue will starve the modulator\n",
                         o.tx_rt_prio, std::strerror(rc));
        else
            std::fprintf(stderr, "bridge: TX thread at SCHED_FIFO %d\n", o.tx_rt_prio);
    }

    // Depth is a TIME budget, not a packet count: 256 frames of 1270 B at
    // 6.96 Mbit/s is 373 ms of queue, 37x a 10 ms target. The budget follows
    // the configured link rate so the bound moves with the radio.
    sdr::TrafficQueues tq(o.q_ctrl_frames, o.q_bulk_frames, o.bulk_every);
    tq.setRateBudget(o.link_bps, o.ctrl_budget_ms, o.bulk_budget_ms);
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
        // The keepalive carries our IDENTITY rather than sixteen zero bytes,
        // and -- via nextControlPayload() -- occasionally an active health
        // probe request or an owed reply instead. All of it is already
        // transmitted continuously to hold the demodulator's timing loop, so
        // none of this costs new protocol or extra airtime: it only ever
        // substitutes for a filler frame idle airtime was already spending.
        // A peer that never hears a HELLO learns nothing, which is why the
        // verdict starts UNKNOWN and is never assumed compatible.
        while (blocks.empty()) {
            auto payload = nextControlPayload(o);
            auto wire = encode(payload.data(), payload.size(), FL_CTRL);
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
                if (g_own_mac_ok && n >= 14 && std::memcmp(pkt.data(), g_own_mac, 6) == 0) {
                    g_to_self.fetch_add(1);      // for this unit's own stack
                    continue;
                }
                bool fwd;
                {
                    std::lock_guard<std::mutex> lk(g_loop_mx);
                    fwd = g_loop_guard.shouldForwardToRadio(
                              pkt.data(), static_cast<size_t>(n), std::chrono::steady_clock::now());
                }
                if (!fwd) { g_stats.loop_suppressed.fetch_add(1); continue; }
                {
                    std::lock_guard<std::mutex> lk(g_loop_mx);
                    g_loop_guard.noteForwarded(pkt.data(), static_cast<size_t>(n),
                                               std::chrono::steady_clock::now());
                }
            }
            // QUEUE, do not transmit yet.
            //
            // Encoding straight into the aggregator is strict arrival order, so
            // a 20-byte command queued behind ten video frames waits for all of
            // them. Queueing first lets the drain below choose what enters the
            // block still being assembled -- the only point where order can
            // still change. A block already handed to the DMA is never
            // reordered.
            if (!tq.push(pkt.data(), static_cast<size_t>(n), o.bulk_min)) {
                // A bounded queue that drops is honest. Growing instead turns a
                // transient overload into unbounded memory on a board with no
                // swap, and into latency rising without limit behind it.
                if (sdr::classify(pkt.data(), static_cast<size_t>(n), o.bulk_min)
                        == sdr::Class::CONTROL) g_stats.q_ctrl_drop.fetch_add(1);
                else                            g_stats.q_bulk_drop.fetch_add(1);
                continue;
            }
            // DRAIN BY PRIORITY into the block still being assembled. Control
            // first, with one slot in eight reserved for bulk so a steady
            // telemetry stream cannot stop video entirely.
            {
                std::vector<uint8_t> qout;
                while (tq.pop(qout)) {
                    auto wire = encode(qout.data(), qout.size(), 0);
                    const size_t before = blocks.size();
                    if (!agg.addFrame(wire, qout.size(), true, blocks)) {
                        g_stats.tx_oversize.fetch_add(1);
                        continue;
                    }
                    if (blocks.size() != before) g_stats.tx_full_flush.fetch_add(1);
                    g_stats.tx_pkts.fetch_add(1);
                    g_stats.tx_bytes.fetch_add(static_cast<uint64_t>(qout.size()));
                    if (!transmit(blocks)) return;
                }
                g_stats.q_ctrl_depth.store(tq.controlDepth());
                g_stats.q_bulk_depth.store(tq.bulkDepth());
                g_stats.q_drain_ms.store(tq.drainMs());
            }
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
        g_stats.rx_boundary.store(deframer.boundaryRecovered());
        for (int i = 0; i < 4; ++i) g_stats.off_hits[i].store(deframer.offsetHits(i));

        for (auto& f : frames) {
            g_stats.rx_frames.fetch_add(1);
            // With coupled radios a unit can hear its own transmitter. Never
            // inject that packet back into its TUN, and do not let it obscure
            // a peer frame that happens to use the same sequence number.
            if (f.node_id == o.node_id) {
                g_stats.rx_self.fetch_add(1);
                // Say so ONCE. Two units left on the default id discard every
                // frame the other sends, silently: frames decode, nothing is
                // delivered, and every other counter looks healthy. That is
                // indistinguishable from a dead link unless this is reported.
                static bool warned_self = false;
                if (!warned_self) {
                    warned_self = true;
                    std::fprintf(stderr,
                        "bridge: WARNING discarding a frame carrying THIS node's id (%u).\n"
                        "bridge:         If the peer is not transmitting into a coupled\n"
                        "bridge:         antenna, the two units share a node id and will\n"
                        "bridge:         never exchange data. Give each --node-id.\n",
                        o.node_id);
                }
                continue;
            }
            // A keepalive is not user data; handing an empty or filler packet
            // to the kernel would be a bug visible only as junk on the wire.
            if (f.flags & FL_CTRL) {
                g_stats.rx_ctrl.fetch_add(1);
                // A control frame may carry the peer's identity. Judging it
                // here means a misconfiguration is NAMED rather than showing up
                // as silence: two units sharing a node id, frequencies that are
                // not crossed, a mismatched rate or ABI all produce total loss
                // with every other counter looking healthy.
                sdr::PeerIdentity pid;
                if (sdr::decodeHello(f.payload.data(), f.payload.size(), pid)) {
                    g_stats.peer_hellos.fetch_add(1);
                    g_stats.peer_last_ms.store((uint64_t)
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    auto chk = sdr::checkPeer(g_me, pid);
                    int v = chk.verdict == sdr::PeerVerdict::COMPATIBLE   ? 1
                          : chk.verdict == sdr::PeerVerdict::INCOMPATIBLE ? 2 : 0;
                    int prev = g_stats.peer_verdict.exchange(v);
                    if (v != prev) {
                        std::lock_guard<std::mutex> lk(g_peer_mx);
                        g_peer_reasons.clear();
                        for (auto& r : chk.reasons) {
                            if (!g_peer_reasons.empty()) g_peer_reasons += "; ";
                            g_peer_reasons += r;
                        }
                        if (v == 2)
                            std::fprintf(stderr, "bridge: PEER_INCOMPATIBLE -- %s\n",
                                         g_peer_reasons.c_str());
                        else if (v == 1)
                            std::fprintf(stderr, "bridge: PEER_COMPATIBLE (node %u)\n",
                                         pid.node_id);
                    }
                }
                // Not a HELLO (rejected on size/magic/version above): try the
                // active health probe next. The two never collide -- see
                // HealthProbe.hpp -- so trying both unconditionally on every
                // FL_CTRL frame is simplest and costs nothing measurable at
                // this rate.
                sdr::ProbeMessage pm;
                sdr::ProbeKind pk = sdr::decodeProbe(f.payload.data(), f.payload.size(), pm);
                if (pk == sdr::ProbeKind::REQUEST) {
                    // Echo it back verbatim (same seq, same sender timestamp
                    // untouched) -- this unit's own clock never enters the
                    // exchange at all, which is the whole reason no time sync
                    // is needed for the sender's RTT to be meaningful.
                    std::lock_guard<std::mutex> lk(g_probe_mx);
                    if (g_probe_pending_replies.size() >= MAX_PENDING_REPLIES) g_probe_pending_replies.pop_front();
                    g_probe_pending_replies.push_back(pm);
                } else if (pk == sdr::ProbeKind::REPLY) {
                    onProbeReply(pm);
                }
                continue;
            }
            if (f.payload.empty()) continue;

            // Learn the source BEFORE injecting: the echo can come back around
            // the switch faster than the next statistics tick. And do not
            // inject a copy of a frame that originated on our own wire at all
            // -- see LoopGuard.hpp, the second rule.
            if (o.raw_eth) {
                if (g_own_mac_ok && f.payload.size() >= 14 &&
                    (std::memcmp(f.payload.data(), g_own_mac, 6) == 0 ||
                     std::memcmp(f.payload.data() + 6, g_own_mac, 6) == 0)) {
                    g_from_self.fetch_add(1);    // our own address came back over the air
                    continue;
                }
                std::lock_guard<std::mutex> lk(g_loop_mx);
                const auto now = std::chrono::steady_clock::now();
                if (!g_loop_guard.shouldInject(f.payload.data(), f.payload.size(), now)) {
                    g_stats.loop_local_copies.fetch_add(1);
                    continue;
                }
                g_loop_guard.learnFromRadio(f.payload.data(), f.payload.size(), now);
            }
            // A FAILED INJECTION MUST BE VISIBLE. This previously did nothing
            // on w <= 0: no counter, no message. A bridge decoding frames
            // perfectly and failing to put a single one on the wire then looks
            // identical to a dead RF link, and the interface's own tx counter
            // -- the only other evidence -- sits at its boot value.
            ssize_t w = ::write(tun_fd, f.payload.data(), f.payload.size());
            if (w <= 0) {
                g_stats.rx_inject_err.fetch_add(1);
                static bool said = false;
                if (!said) {
                    said = true;
                    std::fprintf(stderr,
                        "bridge: WARNING injecting a decoded frame onto %s failed: %s\n"
                        "bridge:         (payload %zu B) -- further failures counted only\n",
                        o.iface.c_str(), std::strerror(errno), f.payload.size());
                }
            }
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
        else if (a == "--tx-kbufs") o.tx_kbufs = std::atoi(next("--tx-kbufs").c_str());
        else if (a == "--tx-pipe-blocks") o.tx_pipe_blocks = std::atoi(next("--tx-pipe-blocks").c_str());
        else if (a == "--tx-rt")   o.tx_rt_prio = std::atoi(next("--tx-rt").c_str());
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
        else if (a == "--probe-interval-s") o.probe_interval_s = std::atoi(next("--probe-interval-s").c_str());
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
    if (o.tx_rt_prio < 0 || o.tx_rt_prio > 90) {
        std::fprintf(stderr, "bridge: --tx-rt must be between 0 and 90\n");
        return 2;
    }
    if (o.tx_pipe_blocks < 1 || o.tx_pipe_blocks > 16) {
        std::fprintf(stderr, "bridge: --tx-pipe-blocks must be between 1 and 16\n");
        return 2;
    }
    if (o.tx_kbufs < 2 || o.tx_kbufs > 16) {
        std::fprintf(stderr, "bridge: --tx-kbufs must be between 2 and 16\n");
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
    // sdr-agent's clear_counters control action sends this. The handler does
    // the one thing that is actually signal-safe (a lock-free atomic store)
    // and nothing else; the stats thread notices it on its own next tick.
    ::signal(SIGUSR1, [](int) { g_clear_counters_requested.store(true); });

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
                             static_cast<size_t>(tx_bytes), o.tx_kbufs)) return 1;
    } else {
        const char* tx_argv[] = { "iio_writedev", "-b", nbuf,
                                  "cf-ad9361-dds-core-lpc", "voltage0", "voltage1", nullptr };
        tx_fd = spawnIio(tx_argv, true, &g_tx_pid, o.pkt * o.tx_pipe_blocks);
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
        std::fprintf(stderr, "bridge: direct libiio TX, buffer=%s samples/%d bytes, %d kernel buffers (RX deferred)\n",
                     tx_nbuf, tx_bytes, o.tx_kbufs);
    else
        std::fprintf(stderr, "bridge: iio_writedev pid=%d buffer=%s samples (RX deferred)\n",
                     (int)g_tx_pid, nbuf);
    }

    int tun_fd = o.raw_eth ? packetOpen(o.iface, o.mtu) : tunOpen(o.iface, o.mtu);
    if (tun_fd < 0) { ::close(tx_fd); ::close(rx_fd); return 1; }
    // Only the TUN path needs addressing; an AF_PACKET bridge deliberately has
    // no IP of its own.
    if (!o.raw_eth && !tunConfigure(o)) { ::close(tun_fd); ::close(tx_fd); ::close(rx_fd); return 1; }

        // Our identity, as advertised in every keepalive. Read from the live
    // registers rather than assumed, so a mismatch between what we believe and
    // what the fabric actually is cannot be advertised as agreement.
    g_me.node_id     = o.node_id;
    g_me.pkt_bytes   = static_cast<uint32_t>(o.pkt);
    g_me.sample_rate = static_cast<uint32_t>(readSysfsU64(
                           "/sys/bus/iio/devices/iio:device0/in_voltage_sampling_frequency"));
    g_me.tx_freq     = static_cast<uint32_t>(readSysfsU64(
                           "/sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_frequency"));
    g_me.rx_freq     = static_cast<uint32_t>(readSysfsU64(
                           "/sys/bus/iio/devices/iio:device0/out_altvoltage0_RX_LO_frequency"));
    g_me.fpga_abi    = static_cast<uint32_t>(readReg32(0x43C50008));
    g_me.fpga_map    = static_cast<uint32_t>(readReg32(0x43C5000C));
    g_me.diff_mode   = static_cast<uint8_t>(readReg32(0x43C10020) & 1u);
    readIfaceMac(o.iface, g_me.mac);

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
            rx_fd = spawnIio(rx_argv, false, &g_rx_pid, o.pkt);
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
    uint64_t last_frames  = g_stats.rx_frames.load()
                          + g_stats.rx_dup.load() + g_stats.rx_crcerr.load();
    DemodStatus last_demod = demodStatus();
    unsigned stalled_intervals = 0;
    // Recovery is rate-limited by doubling, not a one-shot disarm. A stall a
    // reset does not clear must not cause a reset every interval for ever, and
    // must not be abandoned permanently either; backing off reaches a quiet
    // steady state that still retries.
    unsigned recovery_backoff = 0;
    unsigned recovery_penalty = 1;
    // Recovery is armed only after this process has decoded a real/control
    // frame.  With no RF at power-up, noise may exercise the timing loop and
    // increase mu_clamped; that alone must not cause periodic resets.
    // Armed from the start, NOT after the first decoded frame.
    //
    // Arming on first success cannot rescue a COLD stall, and a cold stall is
    // the case that actually occurs: the appliance resets the demodulator
    // during bring-up, before the peer is transmitting, and the core comes up
    // stalled on an idle channel. It then never decodes a frame, so recovery
    // never arms and the link stays dead for ever. Measured: a unit whose
    // demodulator input correlated 0.9969 against the reference at the correct
    // level reported rx 2105 dma, 0 frames, crcerr 0 -- a perfect signal in and
    // nothing out -- with recoveries 0 because the monitor was never armed.
    // Restarting it once the peer was transmitting delivered 48,000 bytes.
    //
    // Arming immediately is safe: firing still requires DMA advancing AND
    // mu_clamped climbing for two consecutive intervals, which an absent signal
    // does not produce, and one attempt still disarms until a frame decodes.
    bool recovery_armed = true;
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
        if (g_clear_counters_requested.exchange(false)) {
            g_stats.clearReportingCounters();
            // probe_sent/delivered/lost live in Stats and are cleared above;
            // the RTT samples backing their percentiles live in this
            // separate, mutex-guarded window and would otherwise keep
            // reporting stale percentiles next to freshly-zeroed counts.
            { std::lock_guard<std::mutex> lk(g_probe_mx); g_probe_rtt_us.clear(); }
            std::fprintf(stderr, "bridge: reporting counters cleared on request\n");
        }
        {
            uint64_t t = ifCounter(o.iface, "tx_dropped");
            uint64_t r = ifCounter(o.iface, "rx_dropped");
            g_stats.tun_tx_drop.store(t > base_tx_drop ? t - base_tx_drop : 0);
            g_stats.tun_rx_drop.store(r > base_rx_drop ? r - base_rx_drop : 0);
        }
        std::fprintf(stderr,
            "bridge: tx %llu pkts / %llu B (idle %llu, err %llu) | "
            "rx %llu dma, %llu frames, %llu B (crcerr %llu, dup %llu, ctrl %llu, self %llu) | "
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
            (unsigned long long)g_stats.rx_self.load(),
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
        if (recovery_backoff > 0) --recovery_backoff;
        const uint64_t dma_delta = dma_now - last_rx_dma;
        // LIVENESS IS ANY DEFRAMER ACTIVITY, not just unique frames delivered.
        // A duplicate proves the demodulator decoded a frame and checked its
        // CRC; so does a CRC failure. Counting only unique deliveries declares a
        // healthy link stalled whenever traffic repeats -- measured here
        // resetting a working demodulator three times while it decoded ~58,000
        // frames, all duplicates of a looping test pattern. Real traffic repeats
        // too: ARP, keepalives, retransmissions, video I-frames.
        const uint64_t live_now = frames_now + g_stats.rx_dup.load()
                                             + g_stats.rx_crcerr.load();
        const uint64_t frame_delta = live_now - last_frames;
        DemodStatus demod_now = demodStatus();
        if (frame_delta > 0) {
            recovery_armed = true;
            stalled_intervals = 0;
            recovery_penalty = 1;   // it worked; forget the backoff
            recovery_backoff = 0;
        } else if (recovery_armed && recovery_backoff == 0) {
            // THE FAULT IS "DMA ADVANCING WITH NO FRAMES". That defines a stalled
            // demodulator and it is what must trigger recovery.
            //
            // The previous trigger also demanded mu_clamped climb by 32 per
            // interval. That signature came from one observed stall and is real,
            // but it is not the only mode: a second stall was measured where
            // lock_count advanced steadily, frames stayed at zero, and mu_clamped
            // moved by ONE in twenty seconds. Keyed on the first mode's symptom,
            // the detector could not see the second by construction. mu_clamped is
            // now logged as corroboration rather than required as a precondition.
            const uint32_t mu_delta = (demod_now.valid && last_demod.valid)
                                    ? (demod_now.mu_clamped - last_demod.mu_clamped) : 0;
            if (dma_delta >= 2) ++stalled_intervals;
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
                // Back off rather than disarm, so a stall one reset does
                // not clear is retried at a decreasing rate.
                recovery_backoff = recovery_penalty;
                if (recovery_penalty < 64) recovery_penalty *= 2;
                stalled_intervals = 0;
            }
        } else {
            stalled_intervals = 0;
        }
        last_rx_dma = dma_now;
        last_frames = live_now;
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
            "%s drops tx %llu rx %llu (since start) | oversize %llu | loopsup %llu | injecterr %llu | qctrl %llu qbulk %llu qdrop %llu/%llu qdrain %llums\n",
            busy_win, busy,
            (unsigned long long)g_stats.decode_us_max.load(),
            (unsigned long long)g_stats.rx_gap_us_max.load(),
            (unsigned long long)g_stats.rx_short.load(),
            (unsigned long long)(g_stats.tx_stall_us_total.load() / 1000),
            o.iface.c_str(),
            (unsigned long long)g_stats.tun_tx_drop.load(),
            (unsigned long long)g_stats.tun_rx_drop.load(),
            (unsigned long long)g_stats.tx_oversize.load(),
            (unsigned long long)g_stats.loop_suppressed.load(),
            (unsigned long long)g_stats.rx_inject_err.load(),
            (unsigned long long)g_stats.q_ctrl_depth.load(),
            (unsigned long long)g_stats.q_bulk_depth.load(),
            (unsigned long long)g_stats.q_ctrl_drop.load(),
            (unsigned long long)g_stats.q_bulk_drop.load(),
            (unsigned long long)g_stats.q_drain_ms.load());
        // A verdict with no recent evidence is STALE, not still true. If the
        // peer's frequency crossing breaks, this unit stops hearing it entirely
        // and would otherwise keep reporting the last good verdict for ever --
        // measured: UNIT-A reported COMPATIBLE indefinitely after UNIT-B was
        // deliberately misconfigured, because no contradicting HELLO can arrive
        // when nothing can be heard at all. Silence is not agreement.
        const uint64_t now_ms = (uint64_t)
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const uint64_t last_ms = g_stats.peer_last_ms.load();
        const uint64_t PEER_STALE_MS = 15000;
        const int pv = g_stats.peer_verdict.load();
        const char* peer_state =
            (last_ms == 0)                     ? "UNKNOWN" :
            (now_ms - last_ms > PEER_STALE_MS) ? "STALE"   :
            (pv == 1) ? "COMPATIBLE" : (pv == 2) ? "INCOMPATIBLE" : "UNKNOWN";

        // Emit the same numbers as MACHINE-READABLE JSON.
        //
        // The line above is for a person reading a log. It is not an interface,
        // and twice now something has re-parsed it and got a plausible wrong
        // answer: a pattern for "rx" also matched "rx gap" and reported 0 where
        // the line said 863, and a pattern containing a slash collided with
        // sed's delimiter and reported null where a number existed. The format
        // was the bug source, not the patterns, so the numbers are published
        // directly here and nothing needs to parse prose.
        //
        // Written to a temp and renamed, so a reader never sees a half-written
        // file.
        // Active health probe percentiles: an exact sort-based percentile
        // over the current rolling window, computed once per tick here --
        // never per-frame -- since the window is small and this is the only
        // place anything reads it.
        uint64_t probe_p50 = 0, probe_p95 = 0, probe_p99 = 0, probe_max = 0;
        std::size_t probe_samples = 0;
        {
            std::vector<uint64_t> rtts;
            { std::lock_guard<std::mutex> lk(g_probe_mx); rtts.assign(g_probe_rtt_us.begin(), g_probe_rtt_us.end()); }
            probe_samples = rtts.size();
            if (!rtts.empty()) {
                std::sort(rtts.begin(), rtts.end());
                probe_p50 = percentile(rtts, 0.50);
                probe_p95 = percentile(rtts, 0.95);
                probe_p99 = percentile(rtts, 0.99);
                probe_max = rtts.back();
            }
        }
        {
            std::string peer_reasons_escaped;
            {
                std::lock_guard<std::mutex> lk(g_peer_mx);
                for (char ch : g_peer_reasons)
                    if (ch == '"' || ch == '\\') { peer_reasons_escaped += '\\'; peer_reasons_escaped += ch; }
                    else if (ch == '\n') peer_reasons_escaped += ' ';
                    else peer_reasons_escaped += ch;
            }
            const char* path = "/tmp/bridge_stats.json";
            const char* tmpp = "/tmp/bridge_stats.json.new";
            if (FILE* jf = std::fopen(tmpp, "w")) {
                std::fprintf(jf,
                    "{\"iface\":\"%s\",\"node_id\":%u,"
                    "\"tx\":{\"packets\":%llu,\"bytes\":%llu,\"idle\":%llu,"
                    "\"errors\":%llu,\"oversize\":%llu,\"stall_ms\":%llu},"
                    "\"rx\":{\"dma\":%llu,\"frames\":%llu,\"bytes\":%llu,"
                    "\"crc_errors\":%llu,\"duplicates\":%llu,\"control\":%llu,"
                    "\"boundary_recovered\":%llu,"
                    "\"self\":%llu,\"inject_err\":%llu},"
                    "\"queues\":{\"control_depth\":%llu,\"bulk_depth\":%llu,"
                    "\"control_drops\":%llu,\"bulk_drops\":%llu,\"drain_ms\":%llu},"
                    "\"loop_guard\":{\"suppressed\":%llu,\"local_copies\":%llu,\"to_self\":%llu,\"from_self\":%llu},"
                    "\"recoveries\":%llu,\"cpu_decode_pct\":%.1f,"
                    "\"peer\":{\"compatibility\":\"%s\",\"hellos\":%llu,\"reasons\":\"%s\"},"
                    "\"probe\":{\"sent\":%llu,\"delivered\":%llu,\"lost\":%llu,"
                    "\"rtt_us\":{\"p50\":%llu,\"p95\":%llu,\"p99\":%llu,\"max\":%llu,\"samples\":%llu}}}\n",
                    o.iface.c_str(), o.node_id,
                    (unsigned long long)g_stats.tx_pkts.load(),
                    (unsigned long long)g_stats.tx_bytes.load(),
                    (unsigned long long)g_stats.tx_idle.load(),
                    (unsigned long long)g_stats.tx_err.load(),
                    (unsigned long long)g_stats.tx_oversize.load(),
                    (unsigned long long)(g_stats.tx_stall_us_total.load() / 1000),
                    (unsigned long long)g_stats.rx_dma.load(),
                    (unsigned long long)g_stats.rx_frames.load(),
                    (unsigned long long)g_stats.rx_bytes.load(),
                    (unsigned long long)g_stats.rx_crcerr.load(),
                    (unsigned long long)g_stats.rx_dup.load(),
                    (unsigned long long)g_stats.rx_ctrl.load(),
                    (unsigned long long)g_stats.rx_boundary.load(),
                    (unsigned long long)g_stats.rx_self.load(),
                    (unsigned long long)g_stats.rx_inject_err.load(),
                    (unsigned long long)g_stats.q_ctrl_depth.load(),
                    (unsigned long long)g_stats.q_bulk_depth.load(),
                    (unsigned long long)g_stats.q_ctrl_drop.load(),
                    (unsigned long long)g_stats.q_bulk_drop.load(),
                    (unsigned long long)g_stats.q_drain_ms.load(),
                    (unsigned long long)g_stats.loop_suppressed.load(),
                    (unsigned long long)g_stats.loop_local_copies.load(),
                    (unsigned long long)g_to_self.load(),
                    (unsigned long long)g_from_self.load(),
                    (unsigned long long)g_stats.recoveries.load(),
                    busy_win,
                    peer_state,
                    (unsigned long long)g_stats.peer_hellos.load(),
                    peer_reasons_escaped.c_str(),
                    (unsigned long long)g_stats.probe_sent.load(),
                    (unsigned long long)g_stats.probe_delivered.load(),
                    (unsigned long long)g_stats.probe_lost.load(),
                    (unsigned long long)probe_p50,
                    (unsigned long long)probe_p95,
                    (unsigned long long)probe_p99,
                    (unsigned long long)probe_max,
                    (unsigned long long)probe_samples);
                std::fclose(jf);
                ::rename(tmpp, path);
            }
        }

        {
            static bool said_stale = false;
            if (std::strcmp(peer_state, "STALE") == 0 && !said_stale) {
                said_stale = true;
                std::fprintf(stderr,
                    "bridge: PEER_STALE -- no HELLO recently. The peer may have stopped,\n"
                    "bridge:        or its frequencies may no longer be crossed with ours,\n"
                    "bridge:        in which case we cannot hear it at all.\n");
            } else if (std::strcmp(peer_state, "STALE") != 0) {
                said_stale = false;
            }
        }
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
