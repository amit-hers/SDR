#include "sdr/transport/TUNTAPDevice.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>         // if_nametoindex
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>  // SIOCBRADDBR, SIOCBRADDIF
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <fstream>
#include <sstream>

namespace sdr {

TUNTAPDevice::~TUNTAPDevice() {
    if (fd_ >= 0) ::close(fd_);
}

std::unique_ptr<TUNTAPDevice> TUNTAPDevice::create(const std::string& name, bool tap) {
    auto dev = std::unique_ptr<TUNTAPDevice>(new TUNTAPDevice());

    dev->fd_ = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (dev->fd_ < 0)
        throw std::runtime_error("TUNTAPDevice: cannot open /dev/net/tun (run as root?)");

    struct ifreq ifr{};
    ifr.ifr_flags = (tap ? IFF_TAP : IFF_TUN) | IFF_NO_PI;
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

    if (::ioctl(dev->fd_, TUNSETIFF, &ifr) < 0)
        throw std::runtime_error("TUNTAPDevice: TUNSETIFF failed");

    dev->name_ = name;

    // Bring interface up
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr2{};
    std::strncpy(ifr2.ifr_name, name.c_str(), IFNAMSIZ - 1);
    ::ioctl(s, SIOCGIFFLAGS, &ifr2);
    ifr2.ifr_flags |= IFF_UP | IFF_RUNNING;
    ::ioctl(s, SIOCSIFFLAGS, &ifr2);
    ::close(s);

    return dev;
}

void TUNTAPDevice::setMTU(int mtu) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;
    ::ioctl(s, SIOCSIFMTU, &ifr);
    ::close(s);
}

void TUNTAPDevice::addToBridge(const std::string& bridge_iface,
                                const std::string& lan_iface) {
    // Create bridge if it doesn't exist
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    ::ioctl(s, SIOCBRADDBR, bridge_iface.c_str());

    // Add TAP to bridge
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, bridge_iface.c_str(), IFNAMSIZ - 1);
    ifr.ifr_ifindex = if_nametoindex(name_.c_str());
    ::ioctl(s, SIOCBRADDIF, &ifr);

    // Add LAN interface to bridge
    ifr.ifr_ifindex = if_nametoindex(lan_iface.c_str());
    ::ioctl(s, SIOCBRADDIF, &ifr);

    // Bring bridge up
    struct ifreq br{};
    std::strncpy(br.ifr_name, bridge_iface.c_str(), IFNAMSIZ - 1);
    ::ioctl(s, SIOCGIFFLAGS, &br);
    br.ifr_flags |= IFF_UP | IFF_RUNNING;
    ::ioctl(s, SIOCSIFFLAGS, &br);

    ::close(s);
}

ssize_t TUNTAPDevice::read(uint8_t* buf, size_t maxlen) {
    return ::read(fd_, buf, maxlen);
}

ssize_t TUNTAPDevice::write(const uint8_t* buf, size_t len) {
    return ::write(fd_, buf, len);
}

namespace {

// Fill a sockaddr_in inside an ifreq/rtentry from dotted-quad text.
bool setAddr(struct sockaddr* sa, const std::string& dotted) {
    auto* in = reinterpret_cast<struct sockaddr_in*>(sa);
    std::memset(in, 0, sizeof *in);
    in->sin_family = AF_INET;
    return ::inet_pton(AF_INET, dotted.c_str(), &in->sin_addr) == 1;
}

std::string maskFromPrefix(int prefix_len) {
    uint32_t m = prefix_len ? htonl(0xFFFFFFFFu << (32 - prefix_len)) : 0;
    char buf[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &m, buf, sizeof buf);
    return buf;
}

} // namespace

void TUNTAPDevice::setIPv4(const std::string& addr, int prefix_len,
                           const std::string& peer) {
    if (prefix_len < 0 || prefix_len > 32)
        throw std::runtime_error("TUNTAPDevice: prefix length must be 0-32");

    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) throw std::runtime_error("TUNTAPDevice: socket for IPv4 config failed");

    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, name_.c_str(), IFNAMSIZ - 1);

    if (!setAddr(&ifr.ifr_addr, addr)) {
        ::close(s);
        throw std::runtime_error("TUNTAPDevice: '" + addr + "' is not an IPv4 address");
    }
    if (::ioctl(s, SIOCSIFADDR, &ifr) < 0) {
        int e = errno; ::close(s);
        throw std::runtime_error("TUNTAPDevice: cannot set address " + addr +
                                 " on " + name_ + ": " + std::strerror(e));
    }

    // A point-to-point destination gives an implicit host route to the far end.
    // Without it the kernel has an address but no idea where the peer lives, and
    // packets for it leave by whatever route matches next -- which on a machine
    // with a wide LAN route is the LAN, silently.
    if (!peer.empty()) {
        struct ifreq d{};
        std::strncpy(d.ifr_name, name_.c_str(), IFNAMSIZ - 1);
        if (!setAddr(&d.ifr_dstaddr, peer)) {
            ::close(s);
            throw std::runtime_error("TUNTAPDevice: '" + peer + "' is not an IPv4 address");
        }
        if (::ioctl(s, SIOCSIFDSTADDR, &d) < 0) {
            int e = errno; ::close(s);
            throw std::runtime_error("TUNTAPDevice: cannot set peer " + peer +
                                     " on " + name_ + ": " + std::strerror(e));
        }
    } else {
        struct ifreq m{};
        std::strncpy(m.ifr_name, name_.c_str(), IFNAMSIZ - 1);
        setAddr(&m.ifr_netmask, maskFromPrefix(prefix_len));
        if (::ioctl(s, SIOCSIFNETMASK, &m) < 0) {
            int e = errno; ::close(s);
            throw std::runtime_error("TUNTAPDevice: cannot set netmask on " +
                                     name_ + ": " + std::strerror(e));
        }
    }

    // Setting an address can clear IFF_UP on some kernels; assert it after.
    struct ifreq f{};
    std::strncpy(f.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    ::ioctl(s, SIOCGIFFLAGS, &f);
    f.ifr_flags |= IFF_UP | IFF_RUNNING;
    ::ioctl(s, SIOCSIFFLAGS, &f);
    ::close(s);
}

std::string TUNTAPDevice::currentIPv4() const {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return {};
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    ifr.ifr_addr.sa_family = AF_INET;
    if (::ioctl(s, SIOCGIFADDR, &ifr) < 0) { ::close(s); return {}; }
    ::close(s);
    char buf[INET_ADDRSTRLEN];
    auto* in = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
    if (!::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf)) return {};
    return buf;
}

bool TUNTAPDevice::addRoute(const std::string& cidr, const std::string& via) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    const std::string net = cidr.substr(0, slash);
    const int prefix = std::atoi(cidr.c_str() + slash + 1);
    if (prefix < 0 || prefix > 32) return false;

    struct rtentry rt{};
    if (!setAddr(&rt.rt_dst, net)) return false;
    if (!setAddr(&rt.rt_genmask, maskFromPrefix(prefix))) return false;
    rt.rt_flags = RTF_UP;
    if (!via.empty()) {
        if (!setAddr(&rt.rt_gateway, via)) return false;
        rt.rt_flags |= RTF_GATEWAY;
    }
    std::string dev = name_;
    rt.rt_dev = const_cast<char*>(dev.c_str());

    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    const bool ok = ::ioctl(s, SIOCADDRT, &rt) == 0;
    ::close(s);
    return ok;
}

bool TUNTAPDevice::conflictsWithExistingRoute(const std::string& cidr,
                                              const std::string& ignore_iface) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    struct in_addr want{};
    if (::inet_pton(AF_INET, cidr.substr(0, slash).c_str(), &want) != 1) return false;
    const int want_pfx = std::atoi(cidr.c_str() + slash + 1);

    // /proc/net/route is hex, little-endian, one line per route.
    std::ifstream f("/proc/net/route");
    if (!f) return false;
    std::string line;
    std::getline(f, line);                       // header
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string iface, dst_hex, gw_hex, flags, refcnt, use, metric, mask_hex;
        is >> iface >> dst_hex >> gw_hex >> flags >> refcnt >> use >> metric >> mask_hex;
        if (iface.empty() || iface == ignore_iface) continue;
        uint32_t dst  = std::strtoul(dst_hex.c_str(),  nullptr, 16);
        uint32_t mask = std::strtoul(mask_hex.c_str(), nullptr, 16);
        if (mask == 0) continue;                 // the default route matches all
        // Overlap either way round: an existing wider route swallows ours, and
        // an existing narrower one is swallowed by it. Both are conflicts.
        uint32_t ours = want.s_addr;
        uint32_t ours_mask = want_pfx ? htonl(0xFFFFFFFFu << (32 - want_pfx)) : 0;
        if ((ours & mask) == (dst & mask) || (ours & ours_mask) == (dst & ours_mask))
            return true;
    }
    return false;
}

} // namespace sdr
