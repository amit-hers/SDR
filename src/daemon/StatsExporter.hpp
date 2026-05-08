#pragma once
#include "sdr/stats/LinkStats.hpp"
#include <string>
#include <thread>
#include <atomic>

namespace sdr {

class StatsExporter {
public:
    StatsExporter(const LinkStats& stats, int interval_ms,
                  const std::string& path = "/tmp/sdr_stats.json");
    ~StatsExporter();

    void start();
    void stop();

private:
    void loop();

    const LinkStats& stats_;
    int              interval_ms_;
    std::string      path_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace sdr
