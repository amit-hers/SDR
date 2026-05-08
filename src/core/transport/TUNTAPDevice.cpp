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

} // namespace sdr
