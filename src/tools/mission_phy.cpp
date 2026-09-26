// Private Unix-datagram adapter for the experimental mission service.
// Uses the existing host PHY; does not change the production FPGA bitstream.
#include "../daemon/Config.hpp"
#include "sdr/phy/SoftwarePhy.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

namespace {
volatile sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
using Clock = std::chrono::steady_clock;
sockaddr_un address(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(a.sun_path)) throw std::runtime_error("socket path length");
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return a;
}
void privateDirectory(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) throw std::runtime_error("absolute socket path required");
    struct stat st{};
    const auto dir = path.substr(0, slash);
    if (lstat(dir.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))
        throw std::runtime_error("socket directory must be private and owned by current user");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "sdr-mission-phy RADIO_CONFIG SOCKET SERVICE_SOCKET CHANNEL_FILE HOME_ID\n"
                     "CHANNEL_FILE: authorized channel_id tx_hz rx_hz, one per line\n";
        return 0;
    }
    if (argc != 6) { std::cerr << "use --help\n"; return 2; }
    int fd = -1, lock = -1;
    bool bound = false;
    try {
        auto cfg = sdr::Config::fromFile(argv[1]);
        if (cfg.encrypt) throw std::runtime_error("mission AEAD replaces legacy AES-CTR; set encrypt false");
        privateDirectory(argv[2]); privateDirectory(argv[3]);
        auto local = address(argv[2]), peer = address(argv[3]);
        lock = open((std::string(argv[2]) + ".lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) throw std::runtime_error("adapter already running or lock unavailable");
        std::map<unsigned, std::pair<double,double>> channels;
        std::ifstream file(argv[4]);
        if (!file) throw std::runtime_error("cannot read authorized channels");
        std::string line;
        while (std::getline(file,line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row(line);
            unsigned id; double tx,rx; std::string extra;
            if (!(row >> id >> tx >> rx) || row >> extra || id > 65535 ||
                !std::isfinite(tx) || !std::isfinite(rx) || tx < 70000000 || tx > 6000000000. ||
                rx < 70000000 || rx > 6000000000. || channels.count(id) || channels.size() >= 16)
                throw std::runtime_error("invalid/duplicate authorized channel");
            channels[id] = {tx,rx};
        }
        size_t consumed = 0;
        const unsigned long parsed_home = std::stoul(argv[5], &consumed);
        if (consumed != std::strlen(argv[5]) || parsed_home > 65535) throw std::runtime_error("invalid home channel");
        const auto home = static_cast<unsigned>(parsed_home);
        if (!channels.count(home)) throw std::runtime_error("home channel absent");
        auto radio = sdr::PlutoSDR::connect(cfg.pluto_ip);
        radio->setTxFrequency(channels.at(home).first);
        radio->setRxFrequency(channels.at(home).second);
        radio->setSampleRate(static_cast<long long>(cfg.bw_mhz) * 1000000 * cfg.samples_per_symbol);
        radio->setBandwidth(static_cast<long long>(cfg.bw_mhz * 1e6 * cfg.rx_bw_factor));
        radio->setTxAttenuation(cfg.tx_atten_db);
        radio->setGainMode(cfg.gain_mode);
        sdr::LinkStats stats;
        sdr::StageProfiler profiler;
        sdr::SoftwarePhy::Params params;
        params.bw_mhz = cfg.bw_mhz; params.samples_per_symbol = cfg.samples_per_symbol;
        params.node_id = cfg.node_id_u32; params.tx_mod = sdr::ModCode::BPSK;
        params.fec = true; params.tx_duty_max = cfg.tx_duty_max;
        params.carrier_sense = cfg.carrier_sense;
        params.carrier_sense_hold_ms = cfg.carrier_sense_hold_ms;
        params.rx_buffer_samples = cfg.rx_buffer_samples; params.rx_queue_depth = cfg.rx_queue_depth;
        std::unique_ptr<sdr::SoftwarePhy> phy;
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("socket failed");
        struct stat existing{};
        if (!lstat(argv[2], &existing)) {
            if (!S_ISSOCK(existing.st_mode)) throw std::runtime_error("refusing to replace non-socket");
            if (unlink(argv[2])) throw std::runtime_error("unlink stale socket failed");
        }
        if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local))) throw std::runtime_error("bind failed");
        bound = true;
        chmod(argv[2],0600);
        auto reply = [&](const std::string& data) {
            (void)sendto(fd,data.data(),data.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
        };
        struct Quality { uint32_t seq=0; bool have=false; uint64_t good=0,lost=0; float evm=-1; };
        std::map<uint32_t,Quality> qualities;
        std::mutex quality_mutex;
        auto handler = [&](const sdr::DecodedFrame& frame) {
            stats.frames_rx_good.fetch_add(1);
            stats.bytes_rx.fetch_add(frame.payload.size());
            {
                std::lock_guard<std::mutex> guard(quality_mutex);
                if (qualities.count(frame.node_id) || qualities.size()<32) {
                    auto& q=qualities[frame.node_id];
                    const uint32_t distance=frame.seq-q.seq;
                    if (q.have && distance>0 && distance<10000) q.lost+=distance-1;
                    if (!q.have || distance>0) { q.seq=frame.seq; ++q.good; }
                    q.have=true; q.evm=stats.evm_rms.load();
                }
            }
            if (frame.payload.size() <= sdr::MAX_PAYLOAD) {
                std::string data("R");
                data.append(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
                reply(data);
            }
        };
        auto start_phy = [&] {
            phy = std::make_unique<sdr::SoftwarePhy>(params, *radio, profiler, stats);
            phy->onFrame(handler);
            phy->start();
        };
        signal(SIGINT,stop); signal(SIGTERM,stop);
        start_phy();
        auto last_heartbeat = Clock::now(), last_stats = Clock::now();
        uint32_t seq = 0;
        unsigned current = home;
        uint64_t old_good=0,old_bad=0,old_seen=0,old_burst=0;
        auto retune = [&](unsigned id) {
            // Recreate all DSP/queue state: no old-channel samples or queued
            // transmissions may cross the channel transaction boundary.
            phy->stop();
            phy.reset();
            try {
                radio->setTxFrequency(channels.at(id).first);
                radio->setRxFrequency(channels.at(id).second);
                start_phy();
                current=id;
            } catch (...) {
                radio->setTxFrequency(channels.at(home).first);
                radio->setRxFrequency(channels.at(home).second);
                current=home;
                throw;
            }
        };
        while (!stopped) {
            pollfd pfd{fd,POLLIN,0};
            (void)poll(&pfd,1,10);
            const auto now = Clock::now();
            if (pfd.revents & POLLIN) {
                uint8_t buffer[sdr::MAX_PAYLOAD+4];
                sockaddr_un source{}; socklen_t source_len=sizeof(source);
                const auto n=recvfrom(fd,buffer,sizeof(buffer),MSG_TRUNC,reinterpret_cast<sockaddr*>(&source),&source_len);
                if (n > 0 && n <= static_cast<ssize_t>(sizeof(buffer)) &&
                    source.sun_family == AF_UNIX && source_len <= sizeof(source) &&
                    std::strncmp(source.sun_path,peer.sun_path,sizeof(source.sun_path)) == 0) {
                    if (buffer[0]=='H' && n==1) { last_heartbeat=now; reply("A"+std::to_string(current)+":"+std::to_string(cfg.node_id_u32)); }
                    else if (buffer[0]=='T' && n>3 && n-3 <= static_cast<ssize_t>(sdr::MAX_PAYLOAD) &&
                             now-last_heartbeat<std::chrono::seconds(2) && buffer[1]>=1 && buffer[1]<=4 && buffer[2]<=1) {
                        if (!phy->sendFrameWithMode(buffer+3, static_cast<size_t>(n-3),
                            buffer[2] ? sdr::FL_FEC : 0, seq++, static_cast<sdr::ModCode>(buffer[1]))) reply("Eframe rejected");
                    } else if (buffer[0]=='C' && n==3) {
                        const unsigned id=unsigned(buffer[1])*256+buffer[2];
                        if (!channels.count(id)) reply("Eunauthorized channel");
                        else { if (id!=current) retune(id); reply("A"+std::to_string(id)+":"+std::to_string(cfg.node_id_u32)); }
                    }
                }
            }
            phy->flushPending();
            if (now-last_heartbeat>std::chrono::seconds(2) && current!=home) { retune(home); reply("A"+std::to_string(home)+":"+std::to_string(cfg.node_id_u32)); }
            if (now-last_stats>=std::chrono::seconds(1)) {
                const auto good=stats.frames_rx_good.load(), bad=stats.frames_rx_bad.load();
                const auto seen=stats.rx_seen_samples.load(), burst=stats.rx_burst_samples.load();
                const auto total=good-old_good+bad-old_bad;
                const float evm=stats.evm_rms.load();
                std::ostringstream out;
                out << "S{\"per\":";
                if(total) out << double(bad-old_bad)/double(total); else out << "null";
                out << ",\"evm\":";
                if(total && evm>=0 && std::isfinite(evm)) out << evm; else out << "null";
                out << ",\"snr_db\":";
                // EVM-derived estimate, not the legacy RSSI+95 placeholder.
                if(total && evm>0 && std::isfinite(evm)) out << std::clamp(-20.*std::log10(evm),-40.,100.);
                else out << "null";
                out << ",\"occupancy\":";
                if(seen>old_seen) out << std::min(1.,double(burst-old_burst)/double(seen-old_seen)); else out << "null";
                out << ",\"clipping\":" << phy->clippedSamples()
                    << ",\"fec_corrected\":" << stats.fec_corrected.load()
                    << ",\"fec_failed\":" << stats.fec_uncorrectable.load() << "}";
                reply(out.str());
                {
                    std::lock_guard<std::mutex> guard(quality_mutex);
                    for (auto& entry:qualities) {
                        auto& q=entry.second;
                        if (q.good && q.evm>0 && std::isfinite(q.evm)) {
                            std::ostringstream feedback;
                            feedback << "S{\"peer\":" << entry.first
                                     << ",\"per\":" << double(q.lost)/double(q.good+q.lost)
                                     << ",\"evm\":" << q.evm
                                     << ",\"snr_db\":" << std::clamp(-20.*std::log10(q.evm),-40.,100.) << "}";
                            reply(feedback.str());
                        }
                        q.good=0;q.lost=0;
                    }
                }
                old_good=good;old_bad=bad;old_seen=seen;old_burst=burst;last_stats=now;
            }
        }
        phy->stop();
        phy.reset();
        radio->setTxFrequency(channels.at(home).first);
        radio->setRxFrequency(channels.at(home).second);
        close(fd); fd=-1;
        unlink(argv[2]); bound=false;
        close(lock); lock=-1;
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "sdr-mission-phy: " << e.what() << "\n";
        if(fd>=0) close(fd);
        if(bound) unlink(argv[2]);
        if(lock>=0) close(lock);
        return 1;
    }
}
