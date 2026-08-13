#include "sdr/stats/LinkStats.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>

namespace sdr {

void LinkStats::updatePeer(uint32_t node_id, float rssi, float snr) {
    using clock = std::chrono::steady_clock;
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> lk(peers_mu);
    auto& p       = peers[node_id];
    p.rssi_dbm    = rssi;
    p.snr_db      = snr;
    p.last_seen_ms = now_ms;
    p.frames_rx++;
}

std::string LinkStats::toJSON() const {
    auto f = [](float v, int p = 1) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(p) << v;
        return o.str();
    };

    std::ostringstream j;
    j << "{"
      << "\"frames_tx\":"         << frames_tx.load()          << ","
      << "\"frames_rx_good\":"    << frames_rx_good.load()     << ","
      << "\"frames_rx_bad\":"     << frames_rx_bad.load()      << ","
      << "\"dropped\":"           << dropped.load()            << ","
      << "\"fec_corrected\":"     << fec_corrected.load()      << ","
      << "\"fec_uncorrectable\":" << fec_uncorrectable.load()  << ","
      << "\"arq_acked\":"         << arq_acked.load()          << ","
      << "\"arq_retransmits\":"   << arq_retransmits.load()    << ","
      << "\"arq_dropped\":"       << arq_dropped.load()        << ","
      << "\"bytes_tx\":"          << bytes_tx.load()           << ","
      << "\"bytes_rx\":"          << bytes_rx.load()           << ","
      << "\"rssi_dbm\":"          << f(rssi_dbm.load())        << ","
      << "\"snr_db\":"            << f(snr_db.load())          << ","
      << "\"tx_kbps\":"           << f(tx_kbps.load())         << ","
      << "\"rx_kbps\":"           << f(rx_kbps.load())         << ","
      << "\"dist_km\":"           << f(dist_km.load(), 3)      << ","
      << "\"cur_mod\":"           << cur_mod.load()            << ","
      << "\"temp_c\":"            << f(temp_c.load(), 1)       << ","
      << "\"uptime_s\":"          << uptime_s.load()           << ","
      << "\"spectrum\":[";

    for (int i = 0; i < FFT_BINS; ++i) {
        j << f(spectrum[static_cast<size_t>(i)]);
        if (i < FFT_BINS - 1) j << ",";
    }
    j << "],\"peers\":[";

    {
        std::lock_guard<std::mutex> lk(peers_mu);
        bool first = true;
        for (const auto& [id, p] : peers) {
            if (!first) j << ",";
            first = false;
            j << "{\"node_id\":\"0x" << std::hex << std::setw(8) << std::setfill('0') << id << std::dec
              << "\",\"rssi_dbm\":"   << f(p.rssi_dbm)
              << ",\"snr_db\":"       << f(p.snr_db)
              << ",\"frames_rx\":"    << p.frames_rx
              << ",\"last_seen_ms\":" << p.last_seen_ms
              << "}";
        }
    }

    j << "]}";
    return j.str();
}

} // namespace sdr
