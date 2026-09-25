/* sdr-agent: the on-board management daemon. One small service per unit that
 * owns the HTTP management API and reads what the appliance is doing without
 * touching the datapath: appliance status, bridge counters, FPGA identity,
 * the AD9363's live configuration, supervisor state, and the log.
 *
 * WHY C, DYNAMICALLY LINKED. Its predecessor (diagnostics_api.cpp) was a
 * static C++ ARMv7 binary of ~810 KiB, and the jffs2 partition it has to live
 * in has ~280 KiB free after the bridge. It therefore never persisted: it was
 * copied to /tmp by hand and vanished at every reboot. The board ships a
 * shared glibc (2.41) but no libstdc++, so plain C against the shared libc is
 * the configuration that fits -- tens of KiB, not hundreds.
 *
 * CONTRACT (unchanged from the predecessor, and tested):
 *   - every endpoint requires "Authorization: Bearer <token>"; the token file
 *     must be a regular file, mode 0600, 32..256 characters;
 *   - GET only; anything else is 405; unknown routes 404; missing data 503;
 *   - nothing here runs a shell. The status collector is exec'd directly;
 *     request parameters never select a command, a path or an argument;
 *   - every buffer, wait and child process is bounded.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQUEST     8192
#define MAX_STATUS      (256 * 1024)
#define MAX_METRICS     (128 * 1024)
#define MAX_LOG         (64 * 1024)
#define MAX_SOURCE_LOG  (256 * 1024)
#define MAX_BODY        (1024 * 1024)
#define MAX_EVENTS      256
#define IO_TIMEOUT_MS   2000
#define SERVICE_VERSION 2

// Live telemetry (SSE). Bounded on every axis: a fixed number of concurrent
// subscribers, a fixed sample period, and a fixed maximum session length so a
// client that never disconnects does not pin a slot forever -- it just
// reconnects, which a browser's EventSource does automatically.
#define STREAM_MAX_CLIENTS 4
#define STREAM_PERIOD_MS   1000
#define STREAM_MAX_TICKS   (4 * 3600)      // ~4 hours at 1 Hz
#define SUPERVISOR_EVERY_N_TICKS 10        // supervisor state changes rarely;
                                            // sampling it less often keeps a
                                            // 1 Hz stream cheap even with
                                            // STREAM_MAX_CLIENTS subscribers

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

struct options {
    const char *bind_addr, *token_file, *status_program, *metrics_file,
               *log_file, *conf_file, *iio_dir;
    int port;
};

/* ── bounded string builder ──────────────────────────────────────────────── */
struct sb { char *p; size_t n, cap; int overflow; };

static void sb_init(struct sb *b) { b->p = NULL; b->n = 0; b->cap = 0; b->overflow = 0; }
static void sb_free(struct sb *b) { free(b->p); sb_init(b); }
static void sb_putn(struct sb *b, const char *s, size_t n) {
    if (b->overflow) return;
    if (b->n + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 4096;
        while (want < b->n + n + 1) want *= 2;
        if (want > MAX_BODY + 1) { b->overflow = 1; return; }
        char *q = realloc(b->p, want);
        if (!q) { b->overflow = 1; return; }
        b->p = q; b->cap = want;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_put(struct sb *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putf(struct sb *b, const char *fmt, ...) {
    char tmp[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    sb_putn(b, tmp, (size_t)n);
}
static void sb_json_str(struct sb *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    sb_put(b, "\"");
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  sb_put(b, "\\\""); break;
        case '\\': sb_put(b, "\\\\"); break;
        case '\n': sb_put(b, "\\n"); break;
        case '\r': sb_put(b, "\\r"); break;
        case '\t': sb_put(b, "\\t"); break;
        default:
            if (c < 0x20) { char u[7] = {'\\','u','0','0',hex[c>>4],hex[c&15],0}; sb_put(b, u); }
            else sb_putn(b, (const char *)&c, 1);
        }
    }
    sb_put(b, "\"");
}

/* ── small helpers ───────────────────────────────────────────────────────── */
static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}
static char *ltrim(char *s) { while (*s == ' ' || *s == '\t') ++s; return s; }
static int blank(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!isspace((unsigned char)s[i])) return 0;
    return 1;
}
static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int constant_time_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), n = la > lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; ++i) {
        unsigned ac = i < la ? (unsigned char)a[i] : 0, bc = i < lb ? (unsigned char)b[i] : 0;
        diff |= ac ^ bc;
    }
    return diff == 0;
}

/* The token file is the whole access control, so its handling is strict: a
 * regular file, no group/other bits, one line of 32..256 characters. */
static int load_token(const char *path, char *out, size_t cap, const char **err) {
    struct stat st;
    if (lstat(path, &st) != 0) { *err = "cannot stat token file"; return 0; }
    if (!S_ISREG(st.st_mode)) { *err = "token file is not regular"; return 0; }
    if ((st.st_mode & 077) != 0) { *err = "token file must be mode 0600"; return 0; }
    FILE *f = fopen(path, "r");
    if (!f || !fgets(out, (int)cap, f)) { if (f) fclose(f); *err = "cannot read token file"; return 0; }
    fclose(f);
    rtrim(out);
    char *t = ltrim(out); if (t != out) memmove(out, t, strlen(t) + 1);
    size_t n = strlen(out);
    if (n < 32 || n > 256) { *err = "token must contain 32..256 characters"; return 0; }
    return 1;
}

/* Last `limit` bytes of a file; 0 on any failure. */
static int read_tail(const char *path, size_t limit, struct sb *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    off_t start = st.st_size > (off_t)limit ? st.st_size - (off_t)limit : 0;
    if (lseek(fd, start, SEEK_SET) < 0) { close(fd); return 0; }
    char buf[4096]; ssize_t n;
    size_t total = 0;
    while (total < limit && (n = read(fd, buf, sizeof buf)) > 0) {
        size_t take = (size_t)n; if (total + take > limit) take = limit - total;
        sb_putn(out, buf, take); total += take;
    }
    close(fd);
    return !out->overflow;
}

/* Read one small sysfs/text file into a fixed buffer; 0 when absent. */
static int read_small(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0; rtrim(out);
    return 1;
}

/* ── log ring ────────────────────────────────────────────────────────────── */
/* The API owns a 64 KiB RAM ring of the appliance log and bounds the
 * producer's tmpfs file at 256 KiB: an accidentally noisy producer must not be
 * able to grow /tmp until the appliance is out of memory. Producers hold the
 * inode open with O_APPEND, so truncating it in place does not disconnect
 * them; their next write simply lands at offset zero. */
struct logring { const char *path; char ring[MAX_LOG]; size_t n; ino_t inode; off_t offset; int available; };

static void ring_append(struct logring *r, const char *chunk, size_t n) {
    if (n > MAX_LOG) { chunk += n - MAX_LOG; n = MAX_LOG; }
    if (r->n + n > MAX_LOG) {
        size_t drop = r->n + n - MAX_LOG;
        memmove(r->ring, r->ring + drop, r->n - drop); r->n -= drop;
    }
    memcpy(r->ring + r->n, chunk, n); r->n += n;
}
static void ring_refresh(struct logring *r) {
    struct stat st;
    if (stat(r->path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (r->inode != st.st_ino || st.st_size < r->offset) { r->inode = st.st_ino; r->offset = 0; }
    int fd = open(r->path, O_RDONLY);
    if (fd < 0) return;
    if (lseek(fd, r->offset, SEEK_SET) >= 0) {
        char buf[4096]; ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) { ring_append(r, buf, (size_t)n); r->offset += n; }
    }
    close(fd);
    if (st.st_size > (off_t)MAX_SOURCE_LOG) {
        fd = open(r->path, O_WRONLY | O_TRUNC);
        if (fd >= 0) { close(fd); r->offset = 0; }
    }
    r->available = 1;
}

/* ── child process, bounded ──────────────────────────────────────────────── */
/* Run one fixed executable directly (never through /bin/sh), capture bounded
 * stdout, and kill it if it exceeds the diagnostics deadline. */
static int run_program(const char *program, size_t limit, struct sb *out) {
    int p[2];
    if (pipe(p) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 0; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, STDERR_FILENO);
        close(p[0]); close(p[1]);
        execl(program, program, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    long long deadline = now_ms() + IO_TIMEOUT_MS;
    int eof = 0;
    size_t total = 0;
    while (!eof && total < limit && now_ms() < deadline) {
        struct pollfd fd = { p[0], POLLIN | POLLHUP, 0 };
        int left = (int)(deadline - now_ms());
        int pr = poll(&fd, 1, left < 1 ? 1 : left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        char buf[4096];
        size_t want = sizeof buf; if (want > limit - total) want = limit - total;
        ssize_t n = read(p[0], buf, want);
        if (n > 0) { sb_putn(out, buf, (size_t)n); total += (size_t)n; }
        else if (n == 0) eof = 1;
        else if (errno != EAGAIN && errno != EINTR) eof = 1;
    }
    close(p[0]);
    int status = 0;
    pid_t done = waitpid(pid, &status, WNOHANG);
    while (done == 0 && now_ms() < deadline) { usleep(1000); done = waitpid(pid, &status, WNOHANG); }
    if (done == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return 0; }
    return eof && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           total < limit && !out->overflow && !blank(out->p ? out->p : "", out->n);
}

/* ── minimal JSON navigation ─────────────────────────────────────────────── */
/* Find a top-level member of a JSON object and return its value's extent.
 * Only depth-1 keys are matched, so "peer" inside "bridge" is not confused
 * with a top-level "peer". This is a scanner, not a parser: it walks strings
 * and brackets and nothing else, which is all the sub-views need. */
static int json_member(const char *s, size_t n, const char *key, const char **vp, size_t *vn) {
    size_t i = 0, klen = strlen(key);
    int depth = 0, in_str = 0;
    while (i < n && isspace((unsigned char)s[i])) ++i;
    if (i >= n || s[i] != '{') return 0;
    for (; i < n; ++i) {
        char c = s[i];
        if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = 0; continue; }
        if (c == '"') {
            if (depth == 1 && i + klen + 1 < n && strncmp(s + i + 1, key, klen) == 0 && s[i + klen + 1] == '"') {
                size_t j = i + klen + 2;
                while (j < n && isspace((unsigned char)s[j])) ++j;
                if (j < n && s[j] == ':') {
                    ++j; while (j < n && isspace((unsigned char)s[j])) ++j;
                    size_t start = j; int d = 0, q = 0;
                    for (; j < n; ++j) {
                        char e = s[j];
                        if (q) { if (e == '\\') ++j; else if (e == '"') q = 0; continue; }
                        if (e == '"') q = 1;
                        else if (e == '{' || e == '[') ++d;
                        else if (e == '}' || e == ']') { if (d == 0) break; --d; }
                        else if (e == ',' && d == 0) break;
                    }
                    size_t end = j;
                    while (end > start && isspace((unsigned char)s[end-1])) --end;
                    *vp = s + start; *vn = end - start;
                    return end > start;
                }
            }
            in_str = 1;
        } else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return 0;
}

/* Copy a numeric top-level field as text, or "null". */
static void json_number_field(const char *s, size_t n, const char *key, char *out, size_t cap) {
    const char *v; size_t vn;
    strncpy(out, "null", cap);
    if (!json_member(s, n, key, &v, &vn) || vn == 0 || vn >= cap) return;
    for (size_t i = 0; i < vn; ++i) if (!isdigit((unsigned char)v[i])) return;
    memcpy(out, v, vn); out[vn] = 0;
}

/* ── structured events ───────────────────────────────────────────────────── */
struct event { const char *sev, *sub, *code; char ts[24], peer[24]; const char *line; size_t len; };

static void parenthesized_node(const char *line, size_t len, char *out, size_t cap) {
    strncpy(out, "null", cap);
    const char *p = memmem(line, len, "(node ", 6);
    if (!p) return;
    p += 6;
    const char *e = memchr(p, ')', (size_t)(line + len - p));
    if (!e || e == p || (size_t)(e - p) >= cap) return;
    for (const char *q = p; q < e; ++q) if (!isdigit((unsigned char)*q)) return;
    memcpy(out, p, (size_t)(e - p)); out[e - p] = 0;
}
static int has(const char *line, size_t len, const char *needle) { return memmem(line, len, needle, strlen(needle)) != NULL; }

static void classify(struct event *e, const char *line, size_t len) {
    e->sev = "info"; e->sub = "appliance"; e->code = "APPLIANCE_EVENT";
    strcpy(e->ts, "null"); strcpy(e->peer, "null"); e->line = line; e->len = len;
    size_t n = 0;
    while (n < len && isdigit((unsigned char)line[n])) ++n;
    if (n > 0 && n < len && line[n] == 's' && n < sizeof e->ts) { memcpy(e->ts, line, n); e->ts[n] = 0; }
    if (has(line, len, "SUPERVISOR")) e->sub = "supervisor";
    else if (has(line, len, "PREFLIGHT")) e->sub = "preflight";
    else if (has(line, len, "PEER_")) e->sub = "peer";
    else if (has(line, len, "RECOVERY")) e->sub = "demodulator";

    if (has(line, len, "PEER_COMPATIBLE")) { e->code = "PEER_COMPATIBLE"; parenthesized_node(line, len, e->peer, sizeof e->peer); }
    else if (has(line, len, "PEER_INCOMPATIBLE")) { e->code = "PEER_INCOMPATIBLE"; e->sev = "error"; parenthesized_node(line, len, e->peer, sizeof e->peer); }
    else if (has(line, len, "PEER_STALE")) { e->code = "PEER_STALE"; e->sev = "warning"; }
    else if (has(line, len, "RECOVERY")) { e->code = "DEMOD_RECOVERY"; e->sev = "warning"; }
    else if (has(line, len, "PREFLIGHT FAIL")) { e->code = "PREFLIGHT_FAILED"; e->sev = "error"; }
    else if (has(line, len, "CONFIGURATION REJECTED")) { e->code = "CONFIG_REJECTED"; e->sev = "error"; e->sub = "configuration"; }
    else if (has(line, len, "SUPERVISOR FAULT")) { e->code = "SUPERVISOR_FAULT"; e->sev = "critical"; }
    else if (has(line, len, "SUPERVISOR HUNG") || has(line, len, "HUNG:")) { e->code = "BRIDGE_HUNG"; e->sev = "error"; e->sub = "supervisor"; }
    else if (has(line, len, "SUPERVISOR starting bridge")) e->code = "BRIDGE_START";
}

static void events_json(const char *log, size_t n, const char *node_id, struct sb *out) {
    static struct event ring[MAX_EVENTS];
    size_t count = 0, head = 0;
    size_t i = 0;
    while (i < n) {
        const char *line = log + i;
        const char *nl = memchr(line, '\n', n - i);
        size_t len = nl ? (size_t)(nl - line) : n - i;
        i += len + (nl ? 1 : 0);
        if (has(line, len, "PEER_") || has(line, len, "RECOVERY") || has(line, len, "SUPERVISOR") ||
            has(line, len, "PREFLIGHT") || has(line, len, "FAULT") || has(line, len, "HUNG")) {
            classify(&ring[(head + count) % MAX_EVENTS], line, len);
            if (count < MAX_EVENTS) ++count; else head = (head + 1) % MAX_EVENTS;
        }
    }
    sb_put(out, "{\"events\":[");
    for (size_t k = 0; k < count; ++k) {
        const struct event *e = &ring[(head + k) % MAX_EVENTS];
        if (k) sb_put(out, ",");
        sb_putf(out, "{\"timestamp_monotonic_s\":%s,\"severity\":\"%s\",\"subsystem\":\"%s\",\"code\":\"%s\",\"node_id\":%s,\"peer_id\":%s,\"message\":",
                e->ts, e->sev, e->sub, e->code, node_id, e->peer);
        sb_json_str(out, e->line, e->len);
        sb_put(out, "}");
    }
    sb_put(out, "]}");
}

/* ── radio: requested vs actual ──────────────────────────────────────────── */
/* Report both what the configuration asked for and what the AD9363 is doing,
 * side by side. Debugging from the requested values alone is how a link was
 * once chased for an hour while the LO was at the previous frequency. Every
 * value is read live from sysfs; absent attributes are null, never guessed. */
static int conf_value(const char *conf, const char *key, char *out, size_t cap) {
    FILE *f = fopen(conf, "r");
    if (!f) return 0;
    char line[256]; size_t klen = strlen(key); int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            strncpy(out, line + klen + 1, cap - 1); out[cap - 1] = 0; rtrim(out); found = 1; break;
        }
    }
    fclose(f);
    return found;
}
static void put_conf_number(struct sb *b, const char *conf, const char *key) {
    char v[64];
    if (!conf_value(conf, key, v, sizeof v) || !*v) { sb_put(b, "null"); return; }
    for (char *p = v; *p; ++p) if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-') { sb_put(b, "null"); return; }
    sb_put(b, v);
}
static void put_conf_string(struct sb *b, const char *conf, const char *key) {
    char v[64];
    if (!conf_value(conf, key, v, sizeof v) || !*v) { sb_put(b, "null"); return; }
    sb_json_str(b, v, strlen(v));
}

static int find_phy(const char *iio_dir, char *out, size_t cap) {
    DIR *d = opendir(iio_dir);
    if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "iio:device", 10) != 0) continue;
        char path[512], name[64];
        snprintf(path, sizeof path, "%s/%s/name", iio_dir, e->d_name);
        if (read_small(path, name, sizeof name) && strcmp(name, "ad9361-phy") == 0) {
            snprintf(out, cap, "%s/%s", iio_dir, e->d_name); found = 1;
        }
    }
    closedir(d);
    return found;
}
/* Attribute as a JSON number: "47.000000 dB" -> 47.000000, "fdd" -> null. */
static void put_attr_number(struct sb *b, const char *dev, const char *attr, double *val) {
    char path[512], v[64];
    snprintf(path, sizeof path, "%s/%s", dev, attr);
    if (val) *val = 0;
    if (!*dev || !read_small(path, v, sizeof v) || !*v) { sb_put(b, "null"); return; }
    char *end; double x = strtod(v, &end);
    if (end == v) { sb_put(b, "null"); return; }
    if (val) *val = x;
    /* print the leading numeric token verbatim: no float re-formatting */
    sb_putn(b, v, (size_t)(end - v));
}
static void put_attr_string(struct sb *b, const char *dev, const char *attr) {
    char path[512], v[64];
    snprintf(path, sizeof path, "%s/%s", dev, attr);
    if (!*dev || !read_small(path, v, sizeof v) || !*v) { sb_put(b, "null"); return; }
    sb_json_str(b, v, strlen(v));
}
static void put_match(struct sb *b, const char *conf, const char *key, double actual, int have_actual, double tol) {
    char v[64];
    if (!have_actual || !conf_value(conf, key, v, sizeof v) || !*v) { sb_put(b, "null"); return; }
    char *end; double want = strtod(v, &end);
    if (end == v) { sb_put(b, "null"); return; }
    double diff = want > actual ? want - actual : actual - want;
    sb_put(b, diff <= tol ? "true" : "false");
}

static void radio_json(const struct options *o, struct sb *b) {
    char dev[512] = "";
    int have = find_phy(o->iio_dir, dev, sizeof dev);
    double tx = 0, rx = 0, rate = 0;

    sb_put(b, "{\"requested\":{\"source\":"); sb_json_str(b, o->conf_file, strlen(o->conf_file));
    sb_put(b, ",\"tx_lo_hz\":");            put_conf_number(b, o->conf_file, "FREQUENCY");
    sb_put(b, ",\"rx_lo_hz\":");            put_conf_number(b, o->conf_file, "RX_FREQUENCY");
    sb_put(b, ",\"sample_rate_hz\":");      put_conf_number(b, o->conf_file, "SAMPLE_RATE");
    sb_put(b, ",\"tx_rf_bandwidth_hz\":");  put_conf_number(b, o->conf_file, "TX_RF_BANDWIDTH");
    sb_put(b, ",\"rx_rf_bandwidth_hz\":");  put_conf_number(b, o->conf_file, "RX_RF_BANDWIDTH");
    sb_put(b, ",\"tx_attenuation_db\":");   put_conf_number(b, o->conf_file, "TX_ATTENUATION_DB");
    sb_put(b, ",\"rx_gain_mode\":");        put_conf_string(b, o->conf_file, "RX_GAIN_MODE");
    sb_put(b, "},\"actual\":{\"source\":");
    if (have) sb_json_str(b, dev, strlen(dev)); else sb_put(b, "null");
    sb_put(b, ",\"tx_lo_hz\":");            put_attr_number(b, dev, "out_altvoltage1_TX_LO_frequency", &tx);
    sb_put(b, ",\"rx_lo_hz\":");            put_attr_number(b, dev, "out_altvoltage0_RX_LO_frequency", &rx);
    sb_put(b, ",\"sample_rate_hz\":");      put_attr_number(b, dev, "in_voltage_sampling_frequency", &rate);
    sb_put(b, ",\"tx_rf_bandwidth_hz\":");  put_attr_number(b, dev, "out_voltage_rf_bandwidth", NULL);
    sb_put(b, ",\"rx_rf_bandwidth_hz\":");  put_attr_number(b, dev, "in_voltage_rf_bandwidth", NULL);
    sb_put(b, ",\"ensm_mode\":");           put_attr_string(b, dev, "ensm_mode");
    sb_put(b, ",\"tx_attenuation_db\":");   put_attr_number(b, dev, "out_voltage0_hardwaregain", NULL);
    sb_put(b, ",\"tx_lo_powerdown\":");     put_attr_number(b, dev, "out_altvoltage1_TX_LO_powerdown", NULL);
    sb_put(b, ",\"rx_gain_mode\":");        put_attr_string(b, dev, "in_voltage0_gain_control_mode");
    sb_put(b, ",\"rx_gain_db\":");          put_attr_number(b, dev, "in_voltage0_hardwaregain", NULL);
    sb_put(b, ",\"rssi_db\":");             put_attr_number(b, dev, "in_voltage0_rssi", NULL);
    sb_put(b, ",\"temp_mc\":");             put_attr_number(b, dev, "in_temp0_input", NULL);
    /* The PLL quantises the LO (444000000 requested reads back 443999998), so
     * "match" allows a small tolerance and says how much. */
    sb_put(b, "},\"match\":{\"tolerance_hz\":1000,\"tx_lo\":");
    put_match(b, o->conf_file, "FREQUENCY", tx, have && tx > 0, 1000);
    sb_put(b, ",\"rx_lo\":");     put_match(b, o->conf_file, "RX_FREQUENCY", rx, have && rx > 0, 1000);
    sb_put(b, ",\"sample_rate\":"); put_match(b, o->conf_file, "SAMPLE_RATE", rate, have && rate > 0, 1000);
    sb_put(b, "}}\n");
}

/* ── HTTP ────────────────────────────────────────────────────────────────── */
static void send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w > 0) { p += w; n -= (size_t)w; }
        else if (w < 0 && errno == EINTR) continue;
        else break;
    }
}
static void reply(int fd, int code, const char *reason, const char *type, const char *body, size_t n) {
    char h[512];
    int hn = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        code, reason, type, n);
    send_all(fd, h, (size_t)hn); send_all(fd, body, n);
}
static void reply_json(int fd, int code, const char *reason, const char *body) { reply(fd, code, reason, "application/json", body, strlen(body)); }
static void reply_sb(int fd, const struct sb *b, const char *type) {
    if (b->overflow) reply_json(fd, 503, "Service Unavailable", "{\"error\":\"response too large\"}\n");
    else reply(fd, 200, "OK", type, b->p ? b->p : "", b->n);
}
static void unavailable(int fd, const char *what) {
    char body[128]; snprintf(body, sizeof body, "{\"error\":\"%s unavailable\"}\n", what);
    reply_json(fd, 503, "Service Unavailable", body);
}

/* A top-level section of a JSON source, or 503 with the section named. */
static void reply_section(int fd, const struct sb *src, int src_ok, const char *key, const char *what) {
    const char *v; size_t vn;
    if (!src_ok || !json_member(src->p, src->n, key, &v, &vn)) { unavailable(fd, what); return; }
    struct sb b; sb_init(&b); sb_putn(&b, v, vn); sb_put(&b, "\n");
    reply_sb(fd, &b, "application/json"); sb_free(&b);
}

static void bundle_json(const struct options *o, const struct logring *logs, struct sb *out) {
    struct sb status, metrics; sb_init(&status); sb_init(&metrics);
    int status_ok = run_program(o->status_program, MAX_STATUS, &status);
    int metrics_ok = read_tail(o->metrics_file, MAX_METRICS, &metrics) && !blank(metrics.p ? metrics.p : "", metrics.n);
    char node[24]; json_number_field(metrics_ok ? metrics.p : "", metrics_ok ? metrics.n : 0, "node_id", node, sizeof node);
    sb_put(out, "{\"bundle_version\":2,\"status\":");
    if (status_ok) { rtrim(status.p); sb_put(out, ltrim(status.p)); } else sb_put(out, "null");
    sb_put(out, ",\"metrics\":");
    if (metrics_ok) { rtrim(metrics.p); sb_put(out, ltrim(metrics.p)); } else sb_put(out, "null");
    sb_put(out, ",\"radio\":");
    { struct sb r; sb_init(&r); radio_json(o, &r); if (r.p) { rtrim(r.p); sb_put(out, r.p); } else sb_put(out, "null"); sb_free(&r); }
    sb_put(out, ",\"structured_events\":");
    if (logs->available) events_json(logs->ring, logs->n, node, out); else sb_put(out, "{\"events\":[]}");
    sb_put(out, ",\"recent_log\":");
    sb_json_str(out, logs->ring, logs->available ? logs->n : 0);
    sb_put(out, "}");
    sb_free(&status); sb_free(&metrics);
}

/* ── Supervisor state, direct: no shell, no status-script fork ──────────────
 * appliance_status.sh derives this by scanning /proc for a process whose
 * /exe is sdr_bridge and one whose /cmdline names appliance_supervise. That
 * is cheap enough to redo directly here without spawning anything, which
 * matters because the streaming endpoint below wants it at least this often
 * for up to STREAM_MAX_CLIENTS concurrent long-lived connections -- forking
 * the whole status script that often would be the expensive path for no
 * benefit, since this is the only section of it the stream needs. */
static void supervisor_json(struct sb *b) {
    int bridges = 0, supervisors = 0;
    DIR *d = opendir("/proc");
    if (!d) { sb_put(b, "null"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[288], link[256];
        snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n > 0) { link[n] = 0; if (strstr(link, "/sdr_bridge")) ++bridges; }
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            char cmd[256]; size_t got = fread(cmd, 1, sizeof cmd - 1, f); fclose(f);
            cmd[got] = 0;
            for (size_t i = 0; i < got; ++i) if (cmd[i] == 0) cmd[i] = ' ';
            if (strstr(cmd, "appliance_supervise")) ++supervisors;
        }
    }
    closedir(d);
    sb_putf(b, "{\"bridges_running\":%d,\"supervisors\":%d}", bridges, supervisors);
}

/* ── Live telemetry (SSE) ────────────────────────────────────────────────
 * An accepted connection that streams forever defeats the anti-DoS pattern
 * every other route in this file uses (one bounded request, one reply, move
 * on): the socket would never be released back to the single-threaded accept
 * loop. So this path forks. The parent returns immediately -- the caller's
 * accept loop keeps serving everyone else -- and the child owns the
 * connection for as long as the client stays subscribed, exiting on the
 * first failed write (the client is gone; this connection never receives
 * anything FROM the browser after its initial GET, so recv() would just
 * block for the connection's lifetime instead) or the bounded tick count.
 *
 * Forking rather than threading keeps every other codepath in this file free
 * of the concurrency review a log ring and token shared across threads would
 * need, at the cost of re-reading each source fresh every tick instead of
 * sharing the parent's cache -- cheap here (one metrics-file tail, two IIO
 * sysfs reads, one /proc scan), so that cost is the right one to pay. */
static pid_t g_stream_pids[STREAM_MAX_CLIENTS];

static void reap_streams(void) {
    for (int i = 0; i < STREAM_MAX_CLIENTS; ++i) {
        if (g_stream_pids[i] <= 0) continue;
        int status;
        if (waitpid(g_stream_pids[i], &status, WNOHANG) > 0) g_stream_pids[i] = 0;
    }
}

static int stream_slot_free(void) {
    reap_streams();
    for (int i = 0; i < STREAM_MAX_CLIENTS; ++i) if (g_stream_pids[i] <= 0) return i;
    return -1;
}

/* One sample: everything the roadmap named that lives in bridge_stats.json
 * already (frames, delivered bytes, CRC errors, duplicates, rx_self, queue
 * depths, peer state, CPU, recoveries) embedded verbatim, plus RSSI and gain
 * read directly from IIO since the bridge does not publish those, plus
 * supervisor state on a slower cadence because it changes far less often
 * than the traffic counters do. */
static void stream_tick(const struct options *o, int tick, struct sb *out) {
    char dev[512] = "";
    int have = find_phy(o->iio_dir, dev, sizeof dev);
    struct sb metrics; sb_init(&metrics);
    int mok = read_tail(o->metrics_file, MAX_METRICS, &metrics) && !blank(metrics.p ? metrics.p : "", metrics.n);

    sb_putf(out, "{\"t\":%lld", now_ms());
    sb_put(out, ",\"rssi_db\":");    put_attr_number(out, have ? dev : "", "in_voltage0_rssi", NULL);
    sb_put(out, ",\"rx_gain_db\":"); put_attr_number(out, have ? dev : "", "in_voltage0_hardwaregain", NULL);
    sb_put(out, ",\"bridge\":");
    if (mok) { size_t n = metrics.n; while (n && (metrics.p[n-1] == '\n' || metrics.p[n-1] == '\r')) --n;
               sb_putn(out, metrics.p, n); }
    else sb_put(out, "null");
    if (tick % SUPERVISOR_EVERY_N_TICKS == 0) {
        sb_put(out, ",\"supervisor\":");
        supervisor_json(out);
    }
    sb_put(out, "}");
    sb_free(&metrics);
}

static void run_stream_child(int fd, int listen_fd, const struct options *o) {
    close(listen_fd);   // this connection's job, not a spare handle on the socket everyone else accepts on
    static const char hdr[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Connection: keep-alive\r\n\r\n";
    send_all(fd, hdr, sizeof hdr - 1);
    for (int tick = 0; tick < STREAM_MAX_TICKS && running; ++tick) {
        struct sb ev; sb_init(&ev);
        stream_tick(o, tick, &ev);
        struct sb frame; sb_init(&frame);
        sb_put(&frame, "data: ");
        sb_putn(&frame, ev.p ? ev.p : "", ev.n);
        sb_put(&frame, "\n\n");
        ssize_t w = send(fd, frame.p ? frame.p : "", frame.n, MSG_NOSIGNAL);
        sb_free(&ev); sb_free(&frame);
        if (w < 0) break;
        struct timespec ts = { STREAM_PERIOD_MS / 1000, (long)(STREAM_PERIOD_MS % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    close(fd);
    _exit(0);
}

static void serve(int fd, int listen_fd, const struct options *o, const char *token, const struct logring *logs) {
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[MAX_REQUEST + 1]; size_t n = 0;
    while (n < MAX_REQUEST) {
        ssize_t r = recv(fd, req + n, MAX_REQUEST - n, 0);
        if (r > 0) { n += (size_t)r; req[n] = 0; if (strstr(req, "\r\n\r\n")) break; }
        else if (r < 0 && errno == EINTR) continue;
        else break;
    }
    req[n] = 0;
    char *hdr_end = strstr(req, "\r\n\r\n");
    if (!hdr_end) { reply_json(fd, 400, "Bad Request", "{\"error\":\"malformed request\"}\n"); return; }
    *hdr_end = 0;

    char *line_end = strstr(req, "\r\n");
    if (line_end) *line_end = 0;
    char method[16] = "", path[512] = "", version[16] = "", extra[2] = "";
    int parts = sscanf(req, "%15s %511s %15s %1s", method, path, version, extra);
    if (parts < 3 || parts > 3 || strncmp(version, "HTTP/1.", 7) != 0) {
        reply_json(fd, 400, "Bad Request", "{\"error\":\"bad request line\"}\n"); return;
    }
    char auth[600] = "";
    for (char *p = line_end ? line_end + 2 : req + strlen(req); p && *p; ) {
        char *e = strstr(p, "\r\n"); if (e) *e = 0;
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            if (strcasecmp(ltrim(p), "authorization") == 0) {
                strncpy(auth, ltrim(colon + 1), sizeof auth - 1); rtrim(auth);
            }
        }
        p = e ? e + 2 : NULL;
    }
    char expected[300]; snprintf(expected, sizeof expected, "Bearer %s", token);
    if (!constant_time_equal(auth, expected)) { reply_json(fd, 401, "Unauthorized", "{\"error\":\"unauthorized\"}\n"); return; }
    if (strcmp(method, "GET") != 0) { reply_json(fd, 405, "Method Not Allowed", "{\"error\":\"read-only API\"}\n"); return; }

    if (strcmp(path, "/api/v1/health") == 0) {
        char body[128]; snprintf(body, sizeof body, "{\"status\":\"ok\",\"service\":\"sdr-agent\",\"version\":%d}\n", SERVICE_VERSION);
        reply_json(fd, 200, "OK", body);
    } else if (strcmp(path, "/api/v1/status") == 0) {
        struct sb b; sb_init(&b);
        if (!run_program(o->status_program, MAX_STATUS, &b)) unavailable(fd, "status");
        else reply_sb(fd, &b, "application/json");
        sb_free(&b);
    } else if (strcmp(path, "/api/v1/fpga") == 0 || strcmp(path, "/api/v1/ethernet") == 0 ||
               strcmp(path, "/api/v1/supervisor") == 0 || strcmp(path, "/api/v1/modem") == 0 ||
               strcmp(path, "/api/v1/config") == 0 || strcmp(path, "/api/v1/progress") == 0) {
        struct sb b; sb_init(&b);
        int ok = run_program(o->status_program, MAX_STATUS, &b);
        reply_section(fd, &b, ok, path + 8, path + 8);
        sb_free(&b);
    } else if (strcmp(path, "/api/v1/metrics") == 0 || strcmp(path, "/metrics") == 0 || strcmp(path, "/api/v1/bridge") == 0) {
        struct sb b; sb_init(&b);
        if (!read_tail(o->metrics_file, MAX_METRICS, &b) || blank(b.p ? b.p : "", b.n)) unavailable(fd, "metrics");
        else reply_sb(fd, &b, "application/json");
        sb_free(&b);
    } else if (strcmp(path, "/api/v1/peer") == 0 || strcmp(path, "/api/v1/queues") == 0) {
        struct sb b; sb_init(&b);
        int ok = read_tail(o->metrics_file, MAX_METRICS, &b) && !blank(b.p ? b.p : "", b.n);
        reply_section(fd, &b, ok, path + 8, path + 8);
        sb_free(&b);
    } else if (strcmp(path, "/api/v1/radio") == 0) {
        struct sb b; sb_init(&b); radio_json(o, &b); reply_sb(fd, &b, "application/json"); sb_free(&b);
    } else if (strcmp(path, "/api/v1/logs") == 0) {
        if (!logs->available) unavailable(fd, "logs");
        else reply(fd, 200, "OK", "text/plain; charset=utf-8", logs->ring, logs->n);
    } else if (strcmp(path, "/api/v1/events") == 0) {
        if (!logs->available) { unavailable(fd, "events"); return; }
        struct sb m; sb_init(&m);
        int mok = read_tail(o->metrics_file, MAX_METRICS, &m);
        char node[24]; json_number_field(mok ? m.p : "", mok ? m.n : 0, "node_id", node, sizeof node);
        struct sb b; sb_init(&b); events_json(logs->ring, logs->n, node, &b);
        reply_sb(fd, &b, "application/json"); sb_free(&b); sb_free(&m);
    } else if (strcmp(path, "/api/v1/stream") == 0) {
        int slot = stream_slot_free();
        if (slot < 0) { reply_json(fd, 503, "Service Unavailable", "{\"error\":\"too many stream subscribers\"}\n"); return; }
        pid_t pid = fork();
        if (pid < 0) { reply_json(fd, 503, "Service Unavailable", "{\"error\":\"cannot start stream\"}\n"); return; }
        if (pid == 0) { run_stream_child(fd, listen_fd, o); }   // never returns
        g_stream_pids[slot] = pid;
        // Deliberately no reply here: the child already sent the response
        // headers and owns the connection from this point on.
    } else if (strcmp(path, "/api/v1/diagnostic-bundle") == 0) {
        struct sb b; sb_init(&b); bundle_json(o, logs, &b); reply_sb(fd, &b, "application/json"); sb_free(&b);
    } else {
        reply_json(fd, 404, "Not Found", "{\"error\":\"not found\"}\n");
    }
}

static int usage(void) {
    fprintf(stderr, "usage: sdr-agent [--bind IP] [--port N] [--token-file PATH] [--status-program PATH]\n"
                    "                 [--metrics-file PATH] [--log-file PATH] [--conf PATH] [--iio-dir PATH]\n");
    return 2;
}

int main(int argc, char **argv) {
    struct options o = {
        "127.0.0.1", "/mnt/jffs2/agent.token", "/mnt/jffs2/appliance_status.sh",
        "/tmp/bridge_stats.json", "/tmp/appliance.log", "/mnt/jffs2/bridge.conf",
        "/sys/bus/iio/devices", 8088
    };
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (i + 1 >= argc) return usage();
        const char *v = argv[++i];
        if (!strcmp(a, "--bind")) o.bind_addr = v;
        else if (!strcmp(a, "--port")) o.port = atoi(v);
        else if (!strcmp(a, "--token-file")) o.token_file = v;
        else if (!strcmp(a, "--status-program")) o.status_program = v;
        else if (!strcmp(a, "--metrics-file")) o.metrics_file = v;
        else if (!strcmp(a, "--log-file")) o.log_file = v;
        else if (!strcmp(a, "--conf")) o.conf_file = v;
        else if (!strcmp(a, "--iio-dir")) o.iio_dir = v;
        else return usage();
    }
    if (o.port < 1 || o.port > 65535) { fprintf(stderr, "invalid port\n"); return 2; }
    char token[300]; const char *err = NULL;
    if (!load_token(o.token_file, token, sizeof token, &err)) { fprintf(stderr, "sdr-agent: %s\n", err); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)o.port);
    if (inet_pton(AF_INET, o.bind_addr, &addr.sin_addr) != 1) { fprintf(stderr, "invalid bind address\n"); return 2; }
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 8) != 0) {
        fprintf(stderr, "sdr-agent: listen %s:%d: %s\n", o.bind_addr, o.port, strerror(errno)); return 1;
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    printf("sdr-agent: http://%s:%d (authenticated, read-only)\n", o.bind_addr, o.port);
    fflush(stdout);

    static struct logring logs; memset(&logs, 0, sizeof logs); logs.path = o.log_file;
    ring_refresh(&logs);
    while (running) {
        ring_refresh(&logs);
        reap_streams();
        struct pollfd p = { s, POLLIN, 0 };
        int pr = poll(&p, 1, 250);
        if (pr <= 0) continue;
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        ring_refresh(&logs);
        serve(c, s, &o, token, &logs);
        close(c);
    }
    for (int i = 0; i < STREAM_MAX_CLIENTS; ++i)
        if (g_stream_pids[i] > 0) kill(g_stream_pids[i], SIGTERM);
    close(s);
    return 0;
}
