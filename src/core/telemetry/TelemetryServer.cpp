#include "sdr/telemetry/TelemetryServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace sdr {

TelemetryServer::TelemetryServer(Snapshot snap, std::string bind_addr, int port)
    : snap_(std::move(snap)), addr_(std::move(bind_addr)), port_(port) {}

TelemetryServer::~TelemetryServer() { stop(); }

bool TelemetryServer::start() {
    if (running_.load()) return true;

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { std::cerr << "[sdr] telemetry: socket: " << std::strerror(errno) << "\n"; return false; }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, addr_.c_str(), &a.sin_addr) != 1) {
        std::cerr << "[sdr] telemetry: bad bind address '" << addr_ << "'\n";
        ::close(fd_); fd_ = -1; return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        std::cerr << "[sdr] telemetry: bind " << addr_ << ":" << port_ << ": "
                  << std::strerror(errno) << "\n";
        ::close(fd_); fd_ = -1; return false;
    }
    // A receive timeout is what lets stop() be prompt: the loop wakes
    // regularly and checks the flag instead of blocking in recvfrom forever.
    timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    running_.store(true);
    thread_ = std::thread(&TelemetryServer::loop, this);
    std::cout << "[sdr] telemetry on udp://" << addr_ << ":" << port_ << "\n";
    return true;
}

void TelemetryServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void TelemetryServer::loop() {
    while (running_.load()) {
        char req[64];
        sockaddr_in from{};
        socklen_t   flen = sizeof from;
        ssize_t n = ::recvfrom(fd_, req, sizeof req, 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;                       // timeout, or interrupted

        // Only one request word is understood. Anything else is ignored in
        // silence: replying to arbitrary datagrams would make this a reflector.
        if (n < 4 || std::memcmp(req, "STAT", 4) != 0) continue;

        StatePacket s;
        try { if (snap_) snap_(s); }
        catch (...) { continue; }                   // never let a snapshot fault kill the thread

        const auto pkt = s.encode();
        ::sendto(fd_, pkt.data(), pkt.size(), 0,
                 reinterpret_cast<sockaddr*>(&from), flen);
    }
}

} // namespace sdr
