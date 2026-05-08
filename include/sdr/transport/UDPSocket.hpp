#pragma once
#include "ITransport.hpp"
#include <string>
#include <memory>
#include <netinet/in.h>

namespace sdr {

class UDPSocket : public ITransport {
public:
    ~UDPSocket() override;
    UDPSocket(const UDPSocket&)            = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;

    // Bind to local port for RX.
    static std::unique_ptr<UDPSocket> bind(uint16_t port);

    // Set remote address for TX.
    void setRemote(const std::string& ip, uint16_t port);

    ssize_t read (uint8_t* buf, size_t maxlen) override;
    ssize_t write(const uint8_t* buf, size_t len) override;
    int     fd()    const override { return fd_; }
    bool    valid() const override { return fd_ >= 0; }

private:
    UDPSocket() = default;
    int             fd_    {-1};
    sockaddr_in     remote_{};
    bool            has_remote_{false};
};

} // namespace sdr
