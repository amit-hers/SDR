// sdrctl -- configure and interrogate an SDR node from one place.
//
//   sdrctl show                        print the effective configuration
//   sdrctl get  <key>                  read one setting
//   sdrctl set  <key> <value> ...      change settings (validated)
//   sdrctl apply [--node N]            push changes to a RUNNING daemon
//   sdrctl status [--host H]           query live state over the wire
//   sdrctl net show|set ...            the radio's own IP/mask/gateway
//
// Settings are edited SURGICALLY: the file is parsed for one key, that value is
// replaced, and everything else is copied through untouched. Re-serialising
// from a struct would silently drop any field this tool does not know about,
// and config.json carries far more than the handful exposed here.
//
// Values are validated against what the hardware and the DSP actually accept,
// because the failure mode otherwise is a daemon that starts, runs, and quietly
// does the wrong thing -- `bw_mhz` above 2 collapses receive duty cycle, and an
// out-of-range attenuation is clamped by the driver without complaint.
#include "sdr/telemetry/StatePacket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace sdr;

namespace {

const char* DEFAULT_CONFIG = "config.json";

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream o; o << f.rdbuf(); return o.str();
}

bool spit(const std::string& p, const std::string& s) {
    // Write to a temporary and rename: a config truncated by a crash or a full
    // disk is worse than an unchanged one, and this file is read at startup.
    const std::string tmp = p + ".tmp";
    { std::ofstream f(tmp); if (!f) return false; f << s; if (!f) return false; }
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}

std::string trimq(std::string s) {
    auto b = s.find_first_not_of(" \t\r\n\"");
    auto e = s.find_last_not_of(" \t\r\n\",");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// Locate `"key": value` and return the span of the value text.
bool findValue(const std::string& j, const std::string& key, size_t& vb, size_t& ve) {
    const std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    vb = p;
    if (p < j.size() && j[p] == '"') {              // quoted string
        size_t q = j.find('"', p + 1);
        if (q == std::string::npos) return false;
        ve = q + 1;
    } else {                                         // bare number/bool
        size_t q = p;
        while (q < j.size() && j[q] != ',' && j[q] != '\n' && j[q] != '}') ++q;
        ve = q;
        while (ve > vb && std::isspace((unsigned char)j[ve-1])) --ve;
    }
    return true;
}

std::string getKey(const std::string& j, const std::string& key) {
    size_t b, e;
    if (!findValue(j, key, b, e)) return {};
    return trimq(j.substr(b, e - b));
}

bool setKey(std::string& j, const std::string& key, const std::string& literal) {
    size_t b, e;
    if (!findValue(j, key, b, e)) return false;
    j = j.substr(0, b) + literal + j.substr(e);
    return true;
}

// ── validation ────────────────────────────────────────────────────────────
struct Err { std::string msg; };

double needNumber(const std::string& k, const std::string& v) {
    try {
        size_t n = 0;
        double d = std::stod(v, &n);
        if (n != v.size()) throw std::invalid_argument("trailing");
        return d;
    } catch (...) { throw Err{k + ": '" + v + "' is not a number"}; }
}

std::string oneOf(const std::string& k, const std::string& v,
                  const std::vector<std::string>& allowed) {
    for (const auto& a : allowed) if (v == a) return v;
    std::ostringstream o; o << k << ": '" << v << "' is not valid. Use one of:";
    for (const auto& a : allowed) o << " " << a;
    throw Err{o.str()};
}

// Returns the JSON literal to write (quoted for strings, bare for numbers).
std::string validate(const std::string& key, const std::string& val) {
    if (key == "mode")
        return "\"" + oneOf(key, val, {"bridge","mesh","p2p-tx","p2p-rx","scan"}) + "\"";
    if (key == "modulation")
        return "\"" + oneOf(key, val, {"BPSK","QPSK","16QAM","64QAM"}) + "\"";
    if (key == "gain_mode")
        return "\"" + oneOf(key, val, {"fast_attack","slow_attack","manual","hybrid"}) + "\"";
    if (key == "cfo_method")
        return "\"" + oneOf(key, val, {"fft","none","preamble"}) + "\"";
    if (key == "freq_tx_mhz" || key == "freq_rx_mhz") {
        double d = needNumber(key, val);
        // AD9363 tuning range. Outside it the driver clamps silently, so the
        // radio ends up on a frequency nobody asked for.
        if (d < 325.0 || d > 3800.0)
            throw Err{key + ": " + val + " MHz is outside the AD936x range 325-3800 MHz"};
        return val;
    }
    if (key == "bw_mhz") {
        double d = needNumber(key, val);
        if (d != 1 && d != 2 && d != 5 && d != 10 && d != 20)
            throw Err{key + ": must be 1, 2, 5, 10 or 20 (1-2 recommended; above 2 the "
                            "receive duty cycle collapses and fewer frames are delivered)"};
        return val;
    }
    if (key == "samples_per_symbol") {
        double d = needNumber(key, val);
        if (d != 2 && d != 4) throw Err{key + ": must be 2 or 4 (4 is the validated mode)"};
        return val;
    }
    if (key == "tx_atten_db") {
        double d = needNumber(key, val);
        if (d < 0.0 || d > 89.0) throw Err{key + ": must be 0-89 dB"};
        return val;
    }
    if (key == "tx_duty_max") {
        double d = needNumber(key, val);
        if (d < 0.0 || d > 1.0) throw Err{key + ": must be 0-1 (fraction of airtime)"};
        return val;
    }
    if (key == "tap_mtu") {
        double d = needNumber(key, val);
        if (d < 576 || d > 1386)
            throw Err{key + ": must be 576-1386 (MAX_PAYLOAD 1400 less the 14-byte Ethernet header)"};
        return val;
    }
    if (key == "encrypt" || key == "fec" || key == "arq" ||
        key == "carrier_sense" || key == "pin_cores")
        return oneOf(key, val, {"true","false"});
    // Anything else: number stays bare, everything else is quoted.
    try { (void)needNumber(key, val); return val; } catch (...) {}
    return "\"" + val + "\"";
}

// ── commands ──────────────────────────────────────────────────────────────
const std::vector<std::string> SHOWN = {
    "mode","pluto_ip","node_id","freq_tx_mhz","freq_rx_mhz","bw_mhz",
    "samples_per_symbol","modulation","tx_atten_db","gain_mode","tx_duty_max",
    "carrier_sense","tap_iface","tap_mtu","encrypt","fec","arq","stats_path",
    "monitor_port"
};

int cmdShow(const std::string& cfg) {
    const std::string j = slurp(cfg);
    if (j.empty()) { std::cerr << "cannot read " << cfg << "\n"; return 1; }
    std::cout << cfg << ":\n";
    for (const auto& k : SHOWN) {
        std::string v = getKey(j, k);
        if (!v.empty()) std::printf("  %-20s %s\n", k.c_str(), v.c_str());
    }
    return 0;
}

int cmdGet(const std::string& cfg, const std::string& key) {
    const std::string j = slurp(cfg);
    if (j.empty()) { std::cerr << "cannot read " << cfg << "\n"; return 1; }
    std::string v = getKey(j, key);
    if (v.empty()) { std::cerr << "no such key: " << key << "\n"; return 1; }
    std::cout << v << "\n";
    return 0;
}

int cmdSet(const std::string& cfg, const std::vector<std::pair<std::string,std::string>>& kv) {
    std::string j = slurp(cfg);
    if (j.empty()) { std::cerr << "cannot read " << cfg << "\n"; return 1; }
    // Validate EVERY change before writing ANY of them, so a typo in the third
    // setting cannot leave the first two applied and the file half-updated.
    std::vector<std::pair<std::string,std::string>> lits;
    for (const auto& [k, v] : kv) {
        size_t b, e;
        if (!findValue(j, k, b, e)) { std::cerr << "no such key: " << k << "\n"; return 1; }
        try { lits.emplace_back(k, validate(k, v)); }
        catch (const Err& err) { std::cerr << err.msg << "\n"; return 1; }
    }
    for (const auto& [k, lit] : lits) setKey(j, k, lit);
    if (!spit(cfg, j)) { std::cerr << "cannot write " << cfg << "\n"; return 1; }
    for (const auto& [k, lit] : lits) std::cout << "  " << k << " = " << lit << "\n";
    std::cout << "Written. Use 'sdrctl apply' to push to a running daemon.\n";
    return 0;
}

// Push a live change: the daemon re-reads the reload file on SIGUSR2. The path
// must match what the daemon was told via SDR_RELOAD_FILE, which the dashboard
// sets per node -- so the node name has to be given when one was used.
int cmdApply(const std::string& cfg, const std::string& node) {
    const std::string j = slurp(cfg);
    if (j.empty()) { std::cerr << "cannot read " << cfg << "\n"; return 1; }
    const std::string path = node.empty() ? "/tmp/sdr_reload.json"
                                          : "/tmp/sdr_reload_" + node + ".json";
    if (!spit(path, j)) { std::cerr << "cannot write " << path << "\n"; return 1; }

    // Find the daemon(s) by walking /proc, so this works without pidof.
    int signalled = 0;
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            const std::string comm = slurp(std::string("/proc/") + e->d_name + "/comm");
            if (comm.rfind("sdr-datalink", 0) != 0) continue;
            pid_t pid = (pid_t)atoi(e->d_name);
            if (kill(pid, SIGUSR2) == 0) { std::cout << "  signalled pid " << pid << "\n"; ++signalled; }
        }
        closedir(d);
    }
    std::cout << "Wrote " << path << "; " << signalled << " daemon(s) signalled.\n";
    if (!signalled)
        std::cout << "No running daemon found. The file is in place for the next start.\n";
    return 0;
}

int cmdStatus(const std::string& host, int port, bool as_json) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        std::cerr << "bad host: " << host << "\n"; ::close(fd); return 1;
    }
    timeval tv{}; tv.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    const char req[] = "STAT";
    if (::sendto(fd, req, 4, 0, (sockaddr*)&a, sizeof a) < 0) { std::perror("sendto"); ::close(fd); return 1; }

    uint8_t buf[2048];
    ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);
    ::close(fd);
    if (n <= 0) {
        std::cerr << "no reply from " << host << ":" << port << " within 2 s.\n"
                  << "Is the daemon running with telemetry enabled?\n";
        return 1;
    }
    auto s = StatePacket::decode(buf, (size_t)n);
    if (!s) { std::cerr << "reply did not decode (bad magic, length or CRC)\n"; return 1; }

    if (as_json) { std::cout << s->toJSON(); return 0; }
    static const char* MODES[] = {"unknown","bridge","mesh","p2p-tx","p2p-rx","scan"};
    static const char* MODS[]  = {"?","BPSK","QPSK","16QAM","64QAM"};
    std::printf("node 0x%08X  %s  up %us\n", s->node_id,
                s->mode < 6 ? MODES[s->mode] : "unknown", s->uptime_s);
    std::printf("  tx %.3f MHz   rx %.3f MHz   bw %.3f MHz   sps %u   %s   atten %.2f dB\n",
                s->freq_tx_hz/1e6, s->freq_rx_hz/1e6, s->bw_hz/1e6, s->sps,
                s->modulation < 5 ? MODS[s->modulation] : "?", s->tx_atten_cdb/100.0);
    std::printf("  frames tx %llu  rx good %llu  rx bad %llu  dropped %llu\n",
                (unsigned long long)s->frames_tx, (unsigned long long)s->frames_rx_good,
                (unsigned long long)s->frames_rx_bad, (unsigned long long)s->dropped);
    std::printf("  rssi %.2f dBm   snr %.2f dB   tx %u kbps   rx %u kbps   duty %u%%\n",
                s->rssi_cdbm/100.0, s->snr_cdb/100.0, s->tx_kbps, s->rx_kbps, s->tx_duty_pct);
    if (s->fpga_magic)
        std::printf("  fpga 0x%08X v0x%08X  abi %u  regmap %u\n",
                    s->fpga_magic, s->fpga_version, s->fpga_abi, s->regmap_ver);
    return 0;
}

// ── the radio's own network settings ──────────────────────────────────────
// These live in config.txt on the board's USB mass-storage volume. The board
// reads it when the volume is EJECTED and does nothing before that, so this
// writes the file and then says so explicitly rather than implying it took
// effect. Editing /opt/config.txt on the board instead does NOT persist: that
// path is in the ramdisk.
std::string findPlutoVolume() {
    const char* roots[] = {"/media", "/run/media", "/mnt"};
    for (const char* root : roots) {
        DIR* d = opendir(root);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string user = std::string(root) + "/" + e->d_name;
            DIR* d2 = opendir(user.c_str());
            if (!d2) continue;
            while (dirent* e2 = readdir(d2)) {
                if (std::string(e2->d_name).rfind("PlutoSDR", 0) != 0) continue;
                std::string cand = user + "/" + e2->d_name + "/config.txt";
                struct stat st{};
                if (::stat(cand.c_str(), &st) == 0) { closedir(d2); closedir(d); return cand; }
            }
            closedir(d2);
        }
        closedir(d);
    }
    return {};
}

int cmdNet(const std::vector<std::string>& args) {
    std::string path = findPlutoVolume();
    std::string ip, mask, gw, host;
    bool set = !args.empty() && args[0] == "set";
    for (size_t i = 0; i < args.size(); ++i) {
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= args.size()) { std::cerr << what << " needs a value\n"; exit(2); }
            return args[++i];
        };
        if (args[i] == "--ip")       ip   = next("--ip");
        else if (args[i] == "--mask") mask = next("--mask");
        else if (args[i] == "--gw")   gw   = next("--gw");
        else if (args[i] == "--hostname") host = next("--hostname");
        else if (args[i] == "--volume")   path = next("--volume");
    }
    if (path.empty()) {
        std::cerr << "No Pluto mass-storage volume found.\n"
                     "Plug the radio in, or pass --volume /path/to/config.txt\n";
        return 1;
    }
    std::string txt = slurp(path);
    if (txt.empty()) { std::cerr << "cannot read " << path << "\n"; return 1; }

    // The file is FAT with CRLF line endings; a stripped CR makes the board's
    // parser ignore the line, so replacements keep the terminator intact.
    auto replaceField = [&](const std::string& key, const std::string& val) -> bool {
        size_t p = txt.find(key + " = ");
        if (p == std::string::npos) return false;
        size_t e = txt.find('\n', p);
        if (e == std::string::npos) e = txt.size();
        size_t cut = e;
        if (cut > p && txt[cut-1] == '\r') --cut;
        txt = txt.substr(0, p) + key + " = " + val + txt.substr(cut);
        return true;
    };

    if (!set) {
        std::cout << path << ":\n";
        for (const char* k : {"hostname","ipaddr","ipaddr_host","netmask","gateway_eth","ipaddr_eth"}) {
            size_t p = txt.find(std::string(k) + " = ");
            if (p == std::string::npos) continue;
            size_t e = txt.find('\n', p); if (e == std::string::npos) e = txt.size();
            std::string line = txt.substr(p, e - p);
            while (!line.empty() && (line.back() == '\r')) line.pop_back();
            std::cout << "  " << line << "\n";
        }
        return 0;
    }

    int changed = 0;
    auto validIp = [](const std::string& s) {
        in_addr tmp{}; return ::inet_pton(AF_INET, s.c_str(), &tmp) == 1;
    };
    if (!ip.empty())   { if (!validIp(ip))   { std::cerr << "--ip: not an IPv4 address\n"; return 1; }   changed += replaceField("ipaddr", ip); }
    if (!mask.empty()) { if (!validIp(mask)) { std::cerr << "--mask: not an IPv4 netmask\n"; return 1; } changed += replaceField("netmask", mask); }
    if (!gw.empty())   { if (!validIp(gw))   { std::cerr << "--gw: not an IPv4 address\n"; return 1; }   changed += replaceField("gateway_eth", gw); }
    if (!host.empty()) changed += replaceField("hostname", host);
    if (!changed) { std::cerr << "nothing changed (no matching fields, or nothing given)\n"; return 1; }
    if (!spit(path, txt)) { std::cerr << "cannot write " << path << "\n"; return 1; }

    std::cout << "Updated " << path << " (" << changed << " field(s)).\n"
              << "NOT YET APPLIED. The radio reads this only when the volume is ejected:\n"
              << "  udisksctl unmount -b <device>      # or eject it from the file manager\n"
              << "The board then reboots and comes up on the new address.\n";
    return 0;
}

int usage() {
    std::cout <<
    "sdrctl -- configure and interrogate an SDR node\n\n"
    "  sdrctl show [--config F]\n"
    "  sdrctl get <key> [--config F]\n"
    "  sdrctl set <key> <value> [<key> <value> ...] [--config F]\n"
    "  sdrctl apply [--config F] [--node NAME]\n"
    "  sdrctl status [--host IP] [--port N] [--json]\n"
    "  sdrctl net show [--volume F]\n"
    "  sdrctl net set [--ip A] [--mask M] [--gw G] [--hostname H] [--volume F]\n\n"
    "Examples\n"
    "  sdrctl set freq_tx_mhz 434 freq_rx_mhz 439 bw_mhz 1 modulation QPSK\n"
    "  sdrctl apply --node A\n"
    "  sdrctl status --host 127.0.0.1 --json\n"
    "  sdrctl net set --ip 192.168.2.17 --mask 255.255.255.0\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);

    std::string cfg = DEFAULT_CONFIG, node, host = "127.0.0.1";
    int port = 5140; bool as_json = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < rest.size(); ++i) {
        auto need = [&](const char* w) -> std::string {
            if (i + 1 >= rest.size()) { std::cerr << w << " needs a value\n"; exit(2); }
            return rest[++i];
        };
        if      (rest[i] == "--config") cfg  = need("--config");
        else if (rest[i] == "--node")   node = need("--node");
        else if (rest[i] == "--host")   host = need("--host");
        else if (rest[i] == "--port")   port = std::stoi(need("--port"));
        else if (rest[i] == "--json")   as_json = true;
        else if (rest[i] == "-h" || rest[i] == "--help") return usage();
        else pos.push_back(rest[i]);
    }

    if (cmd == "show")   return cmdShow(cfg);
    if (cmd == "get")    { if (pos.empty()) return usage(); return cmdGet(cfg, pos[0]); }
    if (cmd == "set") {
        if (pos.size() < 2 || pos.size() % 2)
            { std::cerr << "set takes key/value pairs\n"; return 2; }
        std::vector<std::pair<std::string,std::string>> kv;
        for (size_t i = 0; i + 1 < pos.size(); i += 2) kv.emplace_back(pos[i], pos[i+1]);
        return cmdSet(cfg, kv);
    }
    if (cmd == "apply")  return cmdApply(cfg, node);
    if (cmd == "status") return cmdStatus(host, port, as_json);
    if (cmd == "net")    return cmdNet(pos.empty() ? std::vector<std::string>{"show"} : pos);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") return usage();
    std::cerr << "unknown command: " << cmd << "\n";
    return usage() ? 2 : 2;
}
