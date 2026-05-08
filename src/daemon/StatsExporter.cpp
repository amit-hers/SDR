#include "StatsExporter.hpp"
#include <fstream>
#include <chrono>

namespace sdr {

StatsExporter::StatsExporter(const LinkStats& stats, int interval_ms,
                             const std::string& path)
    : stats_(stats), interval_ms_(interval_ms), path_(path) {}

StatsExporter::~StatsExporter() { stop(); }

void StatsExporter::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&StatsExporter::loop, this);
}

void StatsExporter::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void StatsExporter::loop() {
    while (running_.load()) {
        {
            std::ofstream f(path_, std::ios::trunc);
            if (f) f << stats_.toJSON();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

} // namespace sdr
