#pragma once
#include "ITransport.hpp"
#include <string>
#include <memory>

namespace sdr {

class TUNTAPDevice : public ITransport {
public:
    ~TUNTAPDevice() override;
    TUNTAPDevice(const TUNTAPDevice&)            = delete;
    TUNTAPDevice& operator=(const TUNTAPDevice&) = delete;

    // tap=true  → TAP (Ethernet frames, for l2bridge)
    // tap=false → TUN (IP packets, for mesh)
    static std::unique_ptr<TUNTAPDevice> create(const std::string& name, bool tap);

    void setMTU(int mtu);
    // Add this TAP to a Linux bridge (br0). Creates the bridge if absent.
    void addToBridge(const std::string& bridge_iface,
                     const std::string& lan_iface);

    ssize_t read (uint8_t* buf, size_t maxlen) override;
    ssize_t write(const uint8_t* buf, size_t len) override;
    int     fd()    const override { return fd_; }
    bool    valid() const override { return fd_ >= 0; }

    const std::string& name() const { return name_; }

private:
    TUNTAPDevice() = default;
    int         fd_  {-1};
    std::string name_;
};

} // namespace sdr
