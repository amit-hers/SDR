#include "sdr/stats/LinkStats.hpp"
#include <sstream>
#include <iomanip>
#include <ctime>

namespace sdr {

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
      << "\"bytes_tx\":"          << bytes_tx.load()           << ","
      << "\"bytes_rx\":"          << bytes_rx.load()           << ","
      << "\"rssi_dbm\":"          << f(rssi_dbm.load())        << ","
      << "\"snr_db\":"            << f(snr_db.load())          << ","
      << "\"tx_kbps\":"           << f(tx_kbps.load())         << ","
      << "\"rx_kbps\":"           << f(rx_kbps.load())         << ","
      << "\"dist_km\":"           << f(dist_km.load(), 3)      << ","
      << "\"cur_mod\":"           << cur_mod.load()            << ","
      << "\"uptime_s\":"          << uptime_s.load()           << ","
      << "\"spectrum\":[";

    for (int i = 0; i < FFT_BINS; ++i) {
        j << f(spectrum[static_cast<size_t>(i)]);
        if (i < FFT_BINS - 1) j << ",";
    }
    j << "]}";
    return j.str();
}

} // namespace sdr
