#pragma once
#include "sdr/telemetry/StatePacket.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace sdr {

// Answers state queries over UDP: a client sends "STAT", this replies with one
// encoded StatePacket.
//
// BOUND TO LOOPBACK BY DEFAULT. The report names the node, its frequencies and
// its traffic volumes, and there is no authentication here -- so exposing it on
// a routable address is a decision an operator makes deliberately, not one this
// class makes for them. Request/response only: it never sends unsolicited, so
// it cannot be used to amplify traffic at a third party.
class TelemetryServer {
public:
    // `snapshot` is called per request, on the server thread, and must fill the
    // packet from whatever the caller considers current. Passing a callback
    // rather than references to LinkStats and Config keeps this class free of
    // both and lets the daemon decide what it is willing to publish.
    using Snapshot = std::function<void(StatePacket&)>;

    TelemetryServer(Snapshot snap, std::string bind_addr = "127.0.0.1", int port = 5140);
    ~TelemetryServer();

    TelemetryServer(const TelemetryServer&)            = delete;
    TelemetryServer& operator=(const TelemetryServer&) = delete;

    // Returns false if the socket could not be opened or bound; telemetry is a
    // convenience, so a failure here must never stop the radio.
    bool start();
    void stop();
    bool running() const { return running_.load(); }

private:
    void loop();

    Snapshot           snap_;
    std::string        addr_;
    int                port_;
    int                fd_{-1};
    std::atomic<bool>  running_{false};
    std::thread        thread_;
};

} // namespace sdr
