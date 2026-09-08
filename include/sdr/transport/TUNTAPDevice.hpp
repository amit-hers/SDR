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

    // ── Layer 3 ───────────────────────────────────────────────────────────
    // Configure this interface for IP so an operator does not have to run `ip`
    // by hand. Doing it here rather than in a setup script matters for two
    // reasons found the hard way. An address applied from outside can be flushed
    // by NetworkManager moments later, after which packets for the peer match
    // no specific route and leave by the DEFAULT route instead -- silently, and
    // looking exactly like a dead radio. Separately, a link subnet that overlaps
    // an existing specific route steals the traffic the same way.
    //
    // `peer` empty gives an ordinary subnet address; non-empty makes it a
    // point-to-point link, which is what a radio hop actually is and which
    // gives a host route to the far end without a separate route call.
    // Throws on failure: an L3 mode that comes up without an address looks
    // healthy and carries nothing.
    void setIPv4(const std::string& addr, int prefix_len,
                 const std::string& peer = "");

    // Route `cidr` (e.g. "192.168.50.0/24") to the far side of this link.
    // Returns false if the kernel refused it, which is normal when the route
    // already exists.
    bool addRoute(const std::string& cidr, const std::string& via = "");

    // True when `cidr` overlaps a route the machine already has, ignoring this
    // interface's own and the default route (everything overlaps that). Checked
    // before configuring, because the failure is silent and looks like a dead
    // radio.
    static bool conflictsWithExistingRoute(const std::string& cidr,
                                           const std::string& ignore_iface);

    // The address currently on this interface, or "" if it has none. Used to
    // confirm configuration STUCK: NetworkManager will take an unmanaged-looking
    // interface and flush it moments after it appears, and the resulting silence
    // is indistinguishable from a radio that is not receiving.
    std::string currentIPv4() const;

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
