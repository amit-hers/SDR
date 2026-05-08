#include "sdr/transport/UDPSocket.hpp"
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>

namespace sdr {

UDPSocket::~UDPSocket() {
    if (fd_ >= 0) ::close(fd_);
}

std::unique_ptr<UDPSocket> UDPSocket::bind(uint16_t port) {
    auto s = std::unique_ptr<UDPSocket>(new UDPSocket());
    s->fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd_ < 0)
        throw std::runtime_error("UDPSocket: socket() failed");

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (::bind(s->fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("UDPSocket: bind() failed on port "
                                 + std::to_string(port));
    return s;
}

void UDPSocket::setRemote(const std::string& ip, uint16_t port) {
    remote_ = {};
    remote_.sin_family = AF_INET;
    remote_.sin_port   = htons(port);
    ::inet_pton(AF_INET, ip.c_str(), &remote_.sin_addr);
    has_remote_ = true;
}

ssize_t UDPSocket::read(uint8_t* buf, size_t maxlen) {
    return ::recv(fd_, buf, maxlen, 0);
}

ssize_t UDPSocket::write(const uint8_t* buf, size_t len) {
    if (!has_remote_) return -1;
    return ::sendto(fd_, buf, len, 0,
                    reinterpret_cast<const sockaddr*>(&remote_), sizeof(remote_));
}

} // namespace sdr
