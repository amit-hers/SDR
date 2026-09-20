// Read-only diagnostics HTTP API for the on-board appliance.
// No framework and no dynamic dependencies: this is cross-built as a static
// ARMv7 binary for the small ramdisk.  Every endpoint is fixed and bounded.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr std::size_t MAX_REQUEST = 8192;
constexpr std::size_t MAX_STATUS  = 256 * 1024;
constexpr std::size_t MAX_METRICS = 128 * 1024;
constexpr std::size_t MAX_LOG     = 64 * 1024;
constexpr std::size_t MAX_SOURCE_LOG = 256 * 1024;
constexpr int IO_TIMEOUT_MS = 2000;
std::atomic<bool> running{true};

struct Options {
    std::string bind{"127.0.0.1"};
    int port{8088};
    std::string token_file{"/mnt/jffs2/diagnostics.token"};
    std::string status_program{"/mnt/jffs2/appliance_status.sh"};
    std::string metrics_file{"/tmp/bridge_stats.json"};
    std::string log_file{"/tmp/appliance.log"};
};

void onSignal(int) { running.store(false); }

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) ++n;
    return s.substr(n);
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    std::size_t n = std::max(a.size(), b.size());
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < n; ++i) {
        unsigned ac = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned bc = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= ac ^ bc;
    }
    return diff == 0;
}

bool loadToken(const std::string& path, std::string& token, std::string& error) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) { error = "cannot stat token file"; return false; }
    if (!S_ISREG(st.st_mode)) { error = "token file is not regular"; return false; }
    if ((st.st_mode & 0077) != 0) { error = "token file must be mode 0600"; return false; }
    std::ifstream f(path);
    if (!f || !std::getline(f, token)) { error = "cannot read token file"; return false; }
    token = trim(token);
    if (token.size() < 32 || token.size() > 256) {
        error = "token must contain 32..256 characters";
        return false;
    }
    return true;
}

std::string readTail(const std::string& path, std::size_t limit, bool& ok) {
    ok = false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto end_pos = f.tellg();
    if (end_pos < 0) return {};
    const std::streamoff end = static_cast<std::streamoff>(end_pos);
    std::streamoff start = end > static_cast<std::streamoff>(limit)
                         ? end - static_cast<std::streamoff>(limit) : std::streamoff{0};
    f.seekg(start);
    std::ostringstream out;
    out << f.rdbuf();
    ok = true;
    return out.str();
}

class LogRing {
public:
    explicit LogRing(std::string path) : path_(std::move(path)) {}

    void refresh() {
        struct stat st{};
        if (::stat(path_.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
        if (inode_ != st.st_ino || st.st_size < offset_) {
            inode_ = st.st_ino;
            offset_ = 0;
        }
        std::ifstream f(path_, std::ios::binary);
        if (!f) return;
        f.seekg(offset_);
        std::string chunk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        offset_ += static_cast<std::streamoff>(chunk.size());
        append(chunk);

        // Producers hold this inode open with O_APPEND. Truncating the same
        // inode bounds tmpfs use without disconnecting their file descriptor;
        // subsequent writes continue at offset zero. The complete recent tail
        // is already retained above before truncation.
        if (st.st_size > static_cast<off_t>(MAX_SOURCE_LOG)) {
            int fd = ::open(path_.c_str(), O_WRONLY | O_TRUNC);
            if (fd >= 0) { ::close(fd); offset_ = 0; }
        }
        available_ = true;
    }

    const std::string& snapshot() const { return ring_; }
    bool available() const { return available_; }

private:
    void append(const std::string& chunk) {
        if (chunk.size() >= MAX_LOG) ring_.assign(chunk.end() - static_cast<std::ptrdiff_t>(MAX_LOG), chunk.end());
        else {
            ring_ += chunk;
            if (ring_.size() > MAX_LOG) ring_.erase(0, ring_.size() - MAX_LOG);
        }
    }

    std::string path_;
    std::string ring_;
    ino_t inode_{0};
    std::streamoff offset_{0};
    bool available_{false};
};

// Run one fixed executable directly (never through /bin/sh), capture bounded
// stdout, and kill it if it exceeds the diagnostics deadline.
bool runStatus(const std::string& program, std::string& output) {
    int p[2];
    if (::pipe(p) != 0) return false;
    pid_t pid = ::fork();
    if (pid < 0) { ::close(p[0]); ::close(p[1]); return false; }
    if (pid == 0) {
        ::dup2(p[1], STDOUT_FILENO);
        int nullfd = ::open("/dev/null", O_WRONLY);
        if (nullfd >= 0) ::dup2(nullfd, STDERR_FILENO);
        ::close(p[0]); ::close(p[1]);
        ::execl(program.c_str(), program.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(p[1]);
    ::fcntl(p[0], F_SETFL, ::fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(IO_TIMEOUT_MS);
    bool eof = false;
    while (!eof && output.size() < MAX_STATUS && std::chrono::steady_clock::now() < deadline) {
        pollfd fd{p[0], POLLIN | POLLHUP, 0};
        int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        int pr = ::poll(&fd, 1, std::max(1, left));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        char buf[4096];
        ssize_t n = ::read(p[0], buf, std::min(sizeof buf, MAX_STATUS - output.size()));
        if (n > 0) output.append(buf, static_cast<std::size_t>(n));
        else if (n == 0) eof = true;
        else if (errno != EAGAIN && errno != EINTR) eof = true;
    }
    ::close(p[0]);
    int status = 0;
    pid_t done = ::waitpid(pid, &status, WNOHANG);
    while (done == 0 && std::chrono::steady_clock::now() < deadline) {
        ::usleep(1000);
        done = ::waitpid(pid, &status, WNOHANG);
    }
    if (done == 0) { ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); return false; }
    return eof && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           output.size() < MAX_STATUS && !trim(output).empty();
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
    }
    return out;
}

struct Event {
    std::string timestamp{"null"};
    std::string severity{"info"};
    std::string subsystem{"appliance"};
    std::string code{"APPLIANCE_EVENT"};
    std::string node_id{"null"};
    std::string peer_id{"null"};
    std::string message;
};

std::string numericField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return "null";
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    auto b = p;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') ++p;
    return p == b ? "null" : json.substr(b, p - b);
}

std::string parenthesizedNode(const std::string& line) {
    auto p = line.find("(node ");
    if (p == std::string::npos) return "null";
    p += 6;
    auto e = line.find(')', p);
    if (e == std::string::npos || e == p) return "null";
    for (auto i = p; i < e; ++i) if (line[i] < '0' || line[i] > '9') return "null";
    return line.substr(p, e - p);
}

Event classifyEvent(const std::string& line, const std::string& node_id) {
    Event e; e.message = line; e.node_id = node_id;
    std::size_t n = 0;
    while (n < line.size() && line[n] >= '0' && line[n] <= '9') ++n;
    if (n > 0 && n < line.size() && line[n] == 's') e.timestamp = line.substr(0, n);
    if (line.find("SUPERVISOR") != std::string::npos) e.subsystem = "supervisor";
    else if (line.find("PREFLIGHT") != std::string::npos) e.subsystem = "preflight";
    else if (line.find("PEER_") != std::string::npos) e.subsystem = "peer";
    else if (line.find("RECOVERY") != std::string::npos) e.subsystem = "demodulator";

    if (line.find("PEER_COMPATIBLE") != std::string::npos) {
        e.code = "PEER_COMPATIBLE"; e.peer_id = parenthesizedNode(line);
    } else if (line.find("PEER_INCOMPATIBLE") != std::string::npos) {
        e.code = "PEER_INCOMPATIBLE"; e.severity = "error"; e.peer_id = parenthesizedNode(line);
    } else if (line.find("PEER_STALE") != std::string::npos) {
        e.code = "PEER_STALE"; e.severity = "warning";
    } else if (line.find("RECOVERY") != std::string::npos) {
        e.code = "DEMOD_RECOVERY"; e.severity = "warning";
    } else if (line.find("PREFLIGHT FAIL") != std::string::npos) {
        e.code = "PREFLIGHT_FAILED"; e.severity = "error";
    } else if (line.find("CONFIGURATION REJECTED") != std::string::npos) {
        e.code = "CONFIG_REJECTED"; e.severity = "error"; e.subsystem = "configuration";
    } else if (line.find("SUPERVISOR FAULT") != std::string::npos || line.find("SUPERVISOR FAULT:") != std::string::npos) {
        e.code = "SUPERVISOR_FAULT"; e.severity = "critical";
    } else if (line.find("SUPERVISOR HUNG") != std::string::npos || line.find("HUNG:") != std::string::npos) {
        e.code = "BRIDGE_HUNG"; e.severity = "error"; e.subsystem = "supervisor";
    } else if (line.find("SUPERVISOR starting bridge") != std::string::npos) {
        e.code = "BRIDGE_START";
    }
    return e;
}

std::string eventsFromLog(const std::string& log, const std::string& node_id) {
    std::istringstream in(log);
    std::string line;
    std::deque<Event> events;
    while (std::getline(in, line)) {
        if (line.find("PEER_") != std::string::npos ||
            line.find("RECOVERY") != std::string::npos ||
            line.find("SUPERVISOR") != std::string::npos ||
            line.find("PREFLIGHT") != std::string::npos ||
            line.find("FAULT") != std::string::npos ||
            line.find("HUNG") != std::string::npos) {
            events.push_back(classifyEvent(line, node_id));
            if (events.size() > 256) events.pop_front();
        }
    }
    std::string out = "{\"events\":[";
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i) out += ',';
        const auto& e = events[i];
        out += "{\"timestamp_monotonic_s\":" + e.timestamp +
               ",\"severity\":\"" + jsonEscape(e.severity) +
               "\",\"subsystem\":\"" + jsonEscape(e.subsystem) +
               "\",\"code\":\"" + jsonEscape(e.code) +
               "\",\"node_id\":" + e.node_id +
               ",\"peer_id\":" + e.peer_id +
               ",\"message\":\"" + jsonEscape(e.message) + "\"}";
    }
    out += "]}";
    return out;
}

std::string diagnosticBundle(const Options& o, const std::string& log, bool log_ok) {
    std::string status, metrics;
    const bool status_ok = runStatus(o.status_program, status);
    bool metrics_ok = false;
    metrics = readTail(o.metrics_file, MAX_METRICS, metrics_ok);
    const std::string node = metrics_ok ? numericField(metrics, "node_id") : "null";
    std::string out = "{\"bundle_version\":1,\"status\":";
    out += status_ok ? trim(status) : "null";
    out += ",\"metrics\":";
    out += metrics_ok && !trim(metrics).empty() ? trim(metrics) : "null";
    out += ",\"structured_events\":";
    out += log_ok ? eventsFromLog(log, node) : "{\"events\":[]}";
    out += ",\"recent_log\":\"" + jsonEscape(log_ok ? log : "") + "\"}";
    return out;
}

void sendAll(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) off += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
}

void reply(int fd, int code, const char* reason, const char* type, const std::string& body) {
    std::ostringstream h;
    h << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Connection: close\r\n\r\n";
    sendAll(fd, h.str()); sendAll(fd, body);
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

void serveClient(int fd, const Options& o, const std::string& token, LogRing& logs) {
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char buf[2048];
    while (req.size() < MAX_REQUEST && req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, buf, std::min(sizeof buf, MAX_REQUEST - req.size()), 0);
        if (n > 0) req.append(buf, static_cast<std::size_t>(n));
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    if (req.find("\r\n\r\n") == std::string::npos) {
        reply(fd, 400, "Bad Request", "application/json", "{\"error\":\"malformed request\"}\n"); return;
    }
    auto first_end = req.find("\r\n");
    std::istringstream first(req.substr(0, first_end));
    std::string method, path, version, extra;
    first >> method >> path >> version >> extra;
    if (method.empty() || path.empty() || !extra.empty() || version.rfind("HTTP/1.", 0) != 0) {
        reply(fd, 400, "Bad Request", "application/json", "{\"error\":\"bad request line\"}\n"); return;
    }
    std::string auth;
    std::size_t pos = first_end + 2;
    while (pos < req.size()) {
        auto end = req.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        auto colon = req.find(':', pos);
        if (colon != std::string::npos && colon < end) {
            std::string name = lower(trim(req.substr(pos, colon - pos)));
            if (name == "authorization") auth = trim(req.substr(colon + 1, end - colon - 1));
        }
        pos = end + 2;
    }
    const std::string expected = "Bearer " + token;
    if (!constantTimeEqual(auth, expected)) {
        reply(fd, 401, "Unauthorized", "application/json", "{\"error\":\"unauthorized\"}\n"); return;
    }
    if (method != "GET") {
        reply(fd, 405, "Method Not Allowed", "application/json", "{\"error\":\"read-only API\"}\n"); return;
    }

    if (path == "/api/v1/health") {
        reply(fd, 200, "OK", "application/json", "{\"status\":\"ok\",\"service\":\"sdr-diagnostics\",\"version\":1}\n");
    } else if (path == "/api/v1/status") {
        std::string body;
        if (!runStatus(o.status_program, body))
            reply(fd, 503, "Service Unavailable", "application/json", "{\"error\":\"status unavailable\"}\n");
        else reply(fd, 200, "OK", "application/json", body);
    } else if (path == "/api/v1/metrics" || path == "/metrics") {
        bool ok = false; std::string body = readTail(o.metrics_file, MAX_METRICS, ok);
        if (!ok || trim(body).empty()) reply(fd, 503, "Service Unavailable", "application/json", "{\"error\":\"metrics unavailable\"}\n");
        else reply(fd, 200, "OK", "application/json", body);
    } else if (path == "/api/v1/logs") {
        if (!logs.available()) reply(fd, 503, "Service Unavailable", "application/json", "{\"error\":\"logs unavailable\"}\n");
        else reply(fd, 200, "OK", "text/plain; charset=utf-8", logs.snapshot());
    } else if (path == "/api/v1/events") {
        if (!logs.available()) reply(fd, 503, "Service Unavailable", "application/json", "{\"error\":\"events unavailable\"}\n");
        else {
            bool metrics_ok = false;
            const std::string metrics = readTail(o.metrics_file, MAX_METRICS, metrics_ok);
            reply(fd, 200, "OK", "application/json",
                  eventsFromLog(logs.snapshot(), metrics_ok ? numericField(metrics, "node_id") : "null"));
        }
    } else if (path == "/api/v1/diagnostic-bundle") {
        reply(fd, 200, "OK", "application/json",
              diagnosticBundle(o, logs.snapshot(), logs.available()));
    } else {
        reply(fd, 404, "Not Found", "application/json", "{\"error\":\"not found\"}\n");
    }
}

bool nextArg(int& i, int argc, char** argv, std::string& value) {
    if (++i >= argc) return false;
    value = argv[i]; return true;
}
} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], v;
        if (a == "--bind" && nextArg(i, argc, argv, v)) o.bind = v;
        else if (a == "--port" && nextArg(i, argc, argv, v)) o.port = std::stoi(v);
        else if (a == "--token-file" && nextArg(i, argc, argv, v)) o.token_file = v;
        else if (a == "--status-program" && nextArg(i, argc, argv, v)) o.status_program = v;
        else if (a == "--metrics-file" && nextArg(i, argc, argv, v)) o.metrics_file = v;
        else if (a == "--log-file" && nextArg(i, argc, argv, v)) o.log_file = v;
        else { std::cerr << "usage: diagnostics_api [--bind IP] [--port N] [--token-file PATH] [--status-program PATH] [--metrics-file PATH] [--log-file PATH]\n"; return 2; }
    }
    if (o.port < 1 || o.port > 65535) { std::cerr << "invalid port\n"; return 2; }
    std::string token, error;
    if (!loadToken(o.token_file, token, error)) { std::cerr << "diagnostics: " << error << "\n"; return 1; }

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { std::perror("socket"); return 1; }
    int one = 1; ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<uint16_t>(o.port));
    if (::inet_pton(AF_INET, o.bind.c_str(), &addr.sin_addr) != 1) { std::cerr << "invalid bind address\n"; return 2; }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 8) != 0) {
        std::cerr << "diagnostics: listen " << o.bind << ':' << o.port << ": " << std::strerror(errno) << "\n"; return 1;
    }
    ::signal(SIGINT, onSignal); ::signal(SIGTERM, onSignal); ::signal(SIGPIPE, SIG_IGN);
    std::cout << "diagnostics: http://" << o.bind << ':' << o.port << " (authenticated, read-only)\n";
    LogRing logs(o.log_file);
    logs.refresh();
    while (running.load()) {
        logs.refresh();
        pollfd p{s, POLLIN, 0};
        int pr = ::poll(&p, 1, 250);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        int c = ::accept(s, nullptr, nullptr);
        if (c < 0) continue;
        logs.refresh();
        serveClient(c, o, token, logs);
        ::close(c);
    }
    ::close(s);
    return 0;
}
