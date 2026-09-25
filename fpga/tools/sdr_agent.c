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
 * CONTRACT (unchanged from the predecessor except where noted, and tested):
 *   - every endpoint requires "Authorization: Bearer <token>"; the token file
 *     must be a regular file, mode 0600, 32..256 characters;
 *   - GET only, except the five actions under /api/v1/control/ and
 *     /api/v1/config, which are POST-only (config additionally accepts GET,
 *     which reads rather than writes) and each additionally require
 *     "?confirm=yes". Every other method on every other route is 405;
 *     unknown routes 404; missing data 503;
 *   - still no shell and no arbitrary file access. bridge.conf is the one
 *     file this service ever writes, and only through validate -> backup ->
 *     atomic replace -> verify -> rollback -- never overwritten with
 *     anything that has not already passed config_schema.sh's own
 *     validation on a candidate file, on disk, before the live one is
 *     touched. The status collector, every control action, and the
 *     validator are all exec'd directly against a fixed program or a fixed,
 *     hand-written script; a request can select WHICH of five known control
 *     actions to run, or supply the body of a config candidate, never a
 *     command, a path or an argument. There is no sixth control action and
 *     no way to add one from a request;
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
// reset_demod.sh holds the reset pulse for two 1-second sleeps (~2000 ms)
// before it ever writes a byte of output; run_program only starts counting
// a program "done" once it has EOF, so IO_TIMEOUT_MS here would race the
// script's own sleeps and report a working reset as a timeout failure. A
// generous multiple, not a value trimmed to the nominal 2000 ms, on the
// standing rule that a threshold worth padding gets real margin rather than
// a shave to the edge.
#define DEMOD_RESET_TIMEOUT_MS (IO_TIMEOUT_MS * 4)
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
               *log_file, *conf_file, *iio_dir, *fault_file, *bundle_dir,
               *demod_reset_script, *config_schema_script, *start_script;
    int port;
    int rf_loss_threshold_s;   // --rf-loss-threshold-s; test-only override of RF_LOSS_THRESHOLD_S
    size_t fault_max_bytes;    // --fault-max-bytes; test-only override of FAULT_JOURNAL_MAX_BYTES
    int bundle_cooldown_s;     // --bundle-cooldown-s; test-only override of BUNDLE_CAPTURE_COOLDOWN_S
    int recovery_window_s;     // --recovery-window-s; test-only override of RECOVERY_WINDOW_S
    int config_verify_timeout_s;   // --config-verify-timeout-s; test-only override of CONFIG_VERIFY_TIMEOUT_S
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
 * output, and kill it if it exceeds its deadline.
 *
 * `argv` is NULL for every diagnostics caller (a bare program name, no
 * arguments -- the historical `run_program` contract, preserved exactly by
 * the wrapper below). Config-apply's validator genuinely needs real
 * arguments ("validate", a path), so this takes an explicit argv instead of
 * hardcoding execl(program, program, NULL).
 *
 * `capture_stderr` also changes what counts as success: the diagnostics
 * collectors (status, reset_demod) are expected to produce real stdout, so
 * blank output there is itself a failure worth reporting as one. The config
 * validator's useful text is its FAILURE message, written to stderr, and it
 * prints nothing to stdout at all on that path -- treating blank output as
 * failure would misreport every validation error as some other kind of
 * failure. Exit status alone is authoritative there. */
static int run_program_ex(const char *program, char *const argv[], size_t limit, struct sb *out,
                           int timeout_ms, int capture_stderr) {
    char *const bare_argv[] = { (char *)program, NULL };
    if (!argv) argv = bare_argv;
    int p[2];
    if (pipe(p) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 0; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        if (capture_stderr) {
            dup2(p[1], STDERR_FILENO);
        } else {
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) dup2(nul, STDERR_FILENO);
        }
        close(p[0]); close(p[1]);
        execv(program, argv);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    long long deadline = now_ms() + timeout_ms;
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
    return eof && WIFEXITED(status) && WEXITSTATUS(status) == 0 && total < limit && !out->overflow &&
           (capture_stderr || !blank(out->p ? out->p : "", out->n));
}

static int run_program(const char *program, size_t limit, struct sb *out, int timeout_ms) {
    return run_program_ex(program, NULL, limit, out, timeout_ms, 0);
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
// Defined with the rest of the fault journal, below; forward-declared here so
// the incremental log scanner (which needs classify(), just above it) can
// call it without the two being reordered relative to their natural grouping.
static void journal_append(const struct event *e);
// Defined with the rest of the automatic diagnostic bundle, below; forward-
// declared for the same reason journal_append is.
static void note_event_for_capture(const struct event *e);
static void maybe_capture_bundle(const struct options *o, const char *trigger, const char *detail);

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

/* First integer found starting right after `needle`, or -1 if `needle` is
 * absent or is not immediately followed by a digit. Used to tell a bridge's
 * FIRST start ("attempt 1 of 5") from a RESTART following a crash or hang
 * ("attempt 2 of 5") -- the same log line either way, distinguished only by
 * this number, and an operator cares very differently about the two. */
static int number_after(const char *line, size_t len, const char *needle) {
    const char *p = memmem(line, len, needle, strlen(needle));
    if (!p) return -1;
    p += strlen(needle);
    const char *end = line + len;
    int v = 0, any = 0;
    while (p < end && isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); ++p; any = 1; }
    return any ? v : -1;
}

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
    else if (has(line, len, "PREFLIGHT FAIL")) { e->code = "PREFLIGHT_FAIL"; e->sev = "error"; }
    else if (has(line, len, "CONFIGURATION REJECTED")) { e->code = "CONFIG_REJECTED"; e->sev = "error"; e->sub = "configuration"; }
    else if (has(line, len, "SUPERVISOR FAULT")) { e->code = "SUPERVISOR_FAULT"; e->sev = "critical"; }
    else if (has(line, len, "SUPERVISOR HUNG") || has(line, len, "HUNG:")) { e->code = "BRIDGE_HUNG"; e->sev = "error"; e->sub = "supervisor"; }
    else if (has(line, len, "SUPERVISOR starting bridge")) {
        /* attempt > 1 in the SAME restart window means the previous instance
         * died or hung and the supervisor is bringing it back up -- a very
         * different signal from the ordinary start at boot. */
        if (number_after(line, len, "attempt ") > 1) { e->code = "BRIDGE_RESTART"; e->sev = "warning"; }
        else e->code = "BRIDGE_START";
    }
}

/* Is this line one classify() actually turns into a named event, rather than
 * an "APPLIANCE_EVENT" fallback? Shared by events_json's full-ring rescan and
 * the incremental persistence scan below, so the two can never disagree about
 * which lines are worth keeping. */
static int is_classifiable(const char *line, size_t len) {
    return has(line, len, "PEER_") || has(line, len, "RECOVERY") || has(line, len, "SUPERVISOR") ||
           has(line, len, "PREFLIGHT") || has(line, len, "FAULT") || has(line, len, "HUNG");
}

/* Incrementally tail the SAME log file LogRing displays, classify each line
 * exactly ONCE as it completes, and persist it. A fresh full-ring rescan
 * (what events_json does for a live request) cannot drive this: it has no
 * memory of which lines it already reported, so the same historical line
 * would be journalled again on every single poll for as long as it stays
 * inside the 64 KiB RAM ring. This keeps its own independent read position --
 * deliberately NOT sharing state with struct logring, which exists to serve
 * a bounded RAM VIEW, a different job from noticing NEW lines exactly once. */
struct fault_scanner { const char *path; ino_t inode; off_t offset; char carry[512]; size_t carry_n; };

static void journal_scan(struct fault_scanner *fs) {
    struct stat st;
    if (stat(fs->path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    // A rotation (LogRing's own truncate-in-place, or anything else) looks
    // identical either way: start over from the top of whatever is there now.
    if (fs->inode != st.st_ino || st.st_size < fs->offset) { fs->inode = st.st_ino; fs->offset = 0; fs->carry_n = 0; }
    int fd = open(fs->path, O_RDONLY);
    if (fd < 0) return;
    if (lseek(fd, fs->offset, SEEK_SET) < 0) { close(fd); return; }

    char buf[4096];
    ssize_t got;
    while ((got = read(fd, buf, sizeof buf)) > 0) {
        fs->offset += got;
        // Prepend the incomplete tail carried over from the last read, so a
        // line split across two 4096-byte reads is still seen whole.
        char joined[sizeof fs->carry + sizeof buf];
        size_t total = fs->carry_n;
        memcpy(joined, fs->carry, total);
        memcpy(joined + total, buf, (size_t)got);
        total += (size_t)got;

        size_t start = 0;
        for (size_t i = 0; i < total; ++i) {
            if (joined[i] != '\n') continue;
            size_t len = i - start;
            const char *line = joined + start;
            if (is_classifiable(line, len)) {
                struct event e;
                classify(&e, line, len);
                journal_append(&e);
                note_event_for_capture(&e);
            }
            start = i + 1;
        }
        // Whatever is left after the last newline becomes next call's carry.
        // A single "line" longer than the carry buffer cannot happen from
        // this producer (every log line is one bounded printf), but bound it
        // defensively anyway rather than trust that forever.
        size_t leftover = total - start;
        if (leftover > sizeof fs->carry) leftover = sizeof fs->carry;
        memcpy(fs->carry, joined + (total - leftover), leftover);
        fs->carry_n = leftover;
    }
    close(fd);
}

/* Seconds since the BOARD booted (/proc/uptime), not since this process
 * started. Log-derived event timestamps already mean this (each producer's
 * `log()` helper stamps `cut -d. -f1 /proc/uptime`); a counter-derived event
 * has no such line to borrow a timestamp from, so it must compute the same
 * quantity itself -- CLOCK_MONOTONIC would instead read as seconds since
 * sdr-agent last (re)started, which is a different, misleading number on a
 * unit whose agent was updated more recently than it last rebooted. */
static long long board_uptime_s(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1;
    double up = -1;
    int got = fscanf(f, "%lf", &up);
    fclose(f);
    return got == 1 ? (long long)up : -1;
}

/* A depth-1 field, but from an ARBITRARY object substring rather than only
 * the top-level document -- json_member is already this general; this just
 * adds parsing the result as an integer, for fields json_number_field cannot
 * reach because they are nested (bridge_stats.json's rx.frames,
 * queues.control_drops, queues.bulk_drops). -1 for absent or non-numeric. */
static long long json_num(const char *s, size_t n, const char *key) {
    const char *v; size_t vn;
    if (!json_member(s, n, key, &v, &vn) || vn == 0) return -1;
    long long r = 0;
    for (size_t i = 0; i < vn; ++i) {
        if (!isdigit((unsigned char)v[i])) return -1;
        r = r * 10 + (v[i] - '0');
    }
    return r;
}

/* ── Persistent bounded fault journal ───────────────────────────────────────
 * /tmp/appliance.log lives on the ramdisk and is gone at the next boot, so
 * "what went wrong before the reboot" is currently unanswerable. This mirrors
 * every event this process already recognises -- both log-classified and
 * counter-derived -- into one small file on jffs2, the one thing on this
 * board that outlives a power cycle. It is a FAULT JOURNAL, not telemetry:
 * only edge-triggered transitions ever reach it (the same handful per hour at
 * most that push_synth/journal_scan below produce), never a per-second
 * counter dump, because this flash has finite write endurance and no swap to
 * fall back on if it is exhausted. */
#define FAULT_JOURNAL_MAX_BYTES (32 * 1024)

// Set once in main() before the accept loop starts; read-only from then on.
// A plain global rather than threading a struct options* through every
// function below -- push_synth, four call sites deep in check_counters, has
// no options parameter of its own, and giving it one only to reach two path
// strings and a byte count would be a wider change for no real benefit.
static const struct options *g_opts;
// Same reasoning as g_opts: the automatic bundle capture below fires from
// deep inside journal_scan/check_counters, neither of which has (or should
// gain) a struct logring parameter just to reach recent_events on the rare
// occasion a fault is captured.
static const struct logring *g_logs;

/* This unit's own node id, read fresh from the metrics file at the moment an
 * event is persisted (not cached) -- it does not change while a bridge runs,
 * but reading it once per RARE event costs nothing and avoids depending on
 * whatever else in this process last happened to read it. */
static void current_node_id(char *out, size_t cap) {
    strncpy(out, "null", cap);
    if (!g_opts) return;
    struct sb m; sb_init(&m);
    if (read_tail(g_opts->metrics_file, MAX_METRICS, &m) && !blank(m.p ? m.p : "", m.n))
        json_number_field(m.p, m.n, "node_id", out, cap);
    sb_free(&m);
}

static void journal_append(const struct event *e) {
    if (!g_opts || !g_opts->fault_file || !*g_opts->fault_file) return;
    char node[24]; current_node_id(node, sizeof node);
    struct sb line; sb_init(&line);
    sb_putf(&line, "{\"timestamp_monotonic_s\":%s,\"severity\":\"%s\",\"subsystem\":\"%s\",\"code\":\"%s\",\"node_id\":%s,\"peer_id\":%s,\"message\":",
            e->ts, e->sev, e->sub, e->code, node, e->peer);
    sb_json_str(&line, e->line, e->len);
    sb_put(&line, "}\n");
    if (line.p && !line.overflow) {
        int fd = open(g_opts->fault_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) { ssize_t w = write(fd, line.p, line.n); (void)w; close(fd); }
    }
    sb_free(&line);

    // Bound it: an append-only file with no cap would eventually consume the
    // whole partition. Kept as whole JSON objects -- truncating at an
    // arbitrary byte would leave a line that starts mid-object, which is
    // exactly the "does not even parse" failure this file exists to avoid
    // handing an operator during an actual fault.
    struct stat st;
    if (stat(g_opts->fault_file, &st) == 0 && (size_t)st.st_size > g_opts->fault_max_bytes) {
        struct sb tail; sb_init(&tail);
        if (read_tail(g_opts->fault_file, g_opts->fault_max_bytes, &tail) && tail.p) {
            const char *nl = memchr(tail.p, '\n', tail.n);
            size_t start = nl ? (size_t)(nl + 1 - tail.p) : 0;
            int fd = open(g_opts->fault_file, O_WRONLY | O_TRUNC | O_CREAT, 0600);
            if (fd >= 0) { ssize_t w = write(fd, tail.p + start, tail.n - start); (void)w; close(fd); }
        }
        sb_free(&tail);
    }
}

/* ── Events synthesised from counters, not classified out of log text ──────
 * PEER_STALE, DEMOD_RECOVERY and the rest all have a line something already
 * prints when they happen. Nothing prints a line for "the receiver stopped
 * making progress" or "a queue just dropped a frame" -- those exist only as
 * numbers in bridge_stats.json that a human would have to notice moving (or
 * not moving) between two samples. This turns that noticing into an event,
 * exactly once per transition, without needing any change to the bridge or
 * the supervisor: the agent watches the same file every client already can. */
#define MAX_SYNTH_EVENTS 32
struct synth_slot { struct event e; char msg[160]; };
static struct synth_slot g_synth[MAX_SYNTH_EVENTS];
static size_t g_synth_count = 0, g_synth_head = 0;

static void push_synth(const char *sev, const char *sub, const char *code, const char *fmt, ...) {
    struct synth_slot *s = &g_synth[(g_synth_head + g_synth_count) % MAX_SYNTH_EVENTS];
    if (g_synth_count < MAX_SYNTH_EVENTS) ++g_synth_count; else g_synth_head = (g_synth_head + 1) % MAX_SYNTH_EVENTS;
    va_list ap; va_start(ap, fmt); vsnprintf(s->msg, sizeof s->msg, fmt, ap); va_end(ap);
    s->e.sev = sev; s->e.sub = sub; s->e.code = code;
    snprintf(s->e.ts, sizeof s->e.ts, "%lld", board_uptime_s());
    strcpy(s->e.peer, "null");
    s->e.line = s->msg; s->e.len = strlen(s->msg);
    journal_append(&s->e);   // every counter-derived event is a fault-history candidate
}

/* RF_LOSS: rx.frames has not advanced in over RF_LOSS_THRESHOLD_S. Same
 * threshold and the same field appliance_status.sh's own progress.verdict
 * (NO_RX_PROGRESS) already uses, so the two never disagree about what
 * "stalled" means. Edge-triggered: fires once when the stall is first
 * noticed, not on every poll for as long as it continues, and again once
 * when it clears.
 *
 * QUEUE_DROP: queues.control_drops or .bulk_drops increased since the last
 * poll. Also edge-triggered, and the two queues are tracked and reported
 * independently since a control-queue drop (a lost handshake/keepalive) and
 * a bulk-queue drop (lost payload under load) mean different things.
 *
 * Both need a PREVIOUS sample to compare against, so the very first poll
 * after the agent starts only establishes a baseline and can never fire --
 * otherwise a unit that already had, say, a few queue drops in its history
 * before this agent happened to start would report a phantom event for
 * numbers that were never new. */
#define RF_LOSS_THRESHOLD_S 120   // matches appliance_status.sh's NO_RX_PROGRESS; see struct options for the test override

static void check_counters(const struct options *o) {
    static long long last_poll_ms = 0;
    static int have_baseline = 0;
    static long long last_frames = -1, last_progress_s = 0;
    static long long last_cdrops = -1, last_bdrops = -1;
    static int rf_loss_active = 0;

    long long now = now_ms();
    if (now - last_poll_ms < 1000) return;   // this state machine only needs ~1 Hz
    last_poll_ms = now;

    struct sb m; sb_init(&m);
    int ok = read_tail(o->metrics_file, MAX_METRICS, &m) && !blank(m.p ? m.p : "", m.n);
    if (!ok) { sb_free(&m); return; }

    const char *rx_v, *q_v; size_t rx_n, q_n;
    long long frames = json_member(m.p, m.n, "rx", &rx_v, &rx_n) ? json_num(rx_v, rx_n, "frames") : -1;
    long long cdrops = -1, bdrops = -1;
    if (json_member(m.p, m.n, "queues", &q_v, &q_n)) {
        cdrops = json_num(q_v, q_n, "control_drops");
        bdrops = json_num(q_v, q_n, "bulk_drops");
    }
    sb_free(&m);

    const long long nows = board_uptime_s();

    if (!have_baseline) {
        last_frames = frames; last_progress_s = nows;
        last_cdrops = cdrops; last_bdrops = bdrops;
        have_baseline = 1;
        return;
    }

    if (frames >= 0) {
        if (frames != last_frames) {
            last_frames = frames; last_progress_s = nows;
            if (rf_loss_active) {
                push_synth("info", "demodulator", "RF_LOSS_CLEARED",
                           "rx.frames advancing again after a stall");
                rf_loss_active = 0;
            }
        } else if (!rf_loss_active && nows - last_progress_s > o->rf_loss_threshold_s) {
            char detail[96];
            snprintf(detail, sizeof detail, "rx.frames has not advanced in %llds (threshold %ds)",
                     nows - last_progress_s, o->rf_loss_threshold_s);
            push_synth("error", "demodulator", "RF_LOSS", "%s", detail);
            maybe_capture_bundle(o, "RF_LOSS", detail);
            rf_loss_active = 1;
        }
    }

    if (cdrops >= 0 && last_cdrops >= 0 && cdrops > last_cdrops)
        push_synth("warning", "bridge", "QUEUE_DROP", "control queue dropped %lld frame(s) (total %lld)",
                   cdrops - last_cdrops, cdrops);
    if (bdrops >= 0 && last_bdrops >= 0 && bdrops > last_bdrops)
        push_synth("warning", "bridge", "QUEUE_DROP", "bulk queue dropped %lld frame(s) (total %lld)",
                   bdrops - last_bdrops, bdrops);
    if (cdrops >= 0) last_cdrops = cdrops;
    if (bdrops >= 0) last_bdrops = bdrops;
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
        if (is_classifiable(line, len)) {
            classify(&ring[(head + count) % MAX_EVENTS], line, len);
            if (count < MAX_EVENTS) ++count; else head = (head + 1) % MAX_EVENTS;
        }
    }
    sb_put(out, "{\"events\":[");
    int first = 1;
    for (size_t k = 0; k < count; ++k) {
        const struct event *e = &ring[(head + k) % MAX_EVENTS];
        if (!first) sb_put(out, ",");
        first = 0;
        sb_putf(out, "{\"timestamp_monotonic_s\":%s,\"severity\":\"%s\",\"subsystem\":\"%s\",\"code\":\"%s\",\"node_id\":%s,\"peer_id\":%s,\"message\":",
                e->ts, e->sev, e->sub, e->code, node_id, e->peer);
        sb_json_str(out, e->line, e->len);
        sb_put(out, "}");
    }
    // Counter-derived events (RF_LOSS, QUEUE_DROP, ...): a second source with
    // no line in the log to have been scanned above. Appended after, in the
    // order they were noticed; each carries its own timestamp, so a caller
    // that wants one strict chronological list can still sort by it.
    for (size_t k = 0; k < g_synth_count; ++k) {
        const struct event *e = &g_synth[(g_synth_head + k) % MAX_SYNTH_EVENTS].e;
        if (!first) sb_put(out, ",");
        first = 0;
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

/* Same comparison put_match makes, as a plain boolean rather than a JSON
 * fragment -- config-apply's verify step needs a yes/no to poll on, not a
 * document to embed. */
static int attr_number(const char *dev, const char *attr, double *val) {
    char path[512], v[64];
    snprintf(path, sizeof path, "%s/%s", dev, attr);
    if (!*dev || !read_small(path, v, sizeof v) || !*v) return 0;
    char *end; double x = strtod(v, &end);
    if (end == v) return 0;
    *val = x; return 1;
}
static int conf_matches_actual(const char *conf, const char *key, const char *dev, const char *attr, double tol) {
    char v[64];
    if (!conf_value(conf, key, v, sizeof v) || !*v) return 1;   // nothing requested, nothing to mismatch
    char *end; double want = strtod(v, &end);
    double actual;
    if (end == v || !attr_number(dev, attr, &actual)) return 0;
    double diff = want > actual ? want - actual : actual - want;
    return diff <= tol;
}
/* Has the AD9363 actually converged on what the (possibly just-applied)
 * bridge.conf asks for? Config-apply's verify step polls this rather than
 * just checking that a bridge process exists: a bridge that came back up
 * against the OLD RF state (a config that validated syntactically but
 * whose radio settings never took, e.g. from a driver that silently
 * clamped something) is not what "apply" was asked to do. */
static int radio_converged(const struct options *o) {
    char dev[512] = "";
    if (!find_phy(o->iio_dir, dev, sizeof dev)) return 0;
    return conf_matches_actual(o->conf_file, "FREQUENCY", dev, "out_altvoltage1_TX_LO_frequency", 1000) &&
           conf_matches_actual(o->conf_file, "RX_FREQUENCY", dev, "out_altvoltage0_RX_LO_frequency", 1000) &&
           conf_matches_actual(o->conf_file, "SAMPLE_RATE", dev, "in_voltage_sampling_frequency", 1000);
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

/* ── Automatic diagnostic bundle ─────────────────────────────────────────────
 * An operator has to already suspect something is wrong, and be looking at
 * the right unit at the right moment, for /api/v1/diagnostic-bundle to catch
 * anything -- by the time someone asks, the transient condition that mattered
 * may be long gone. This captures the SAME kind of snapshot itself, the
 * moment it recognises a fault, and keeps a bounded few of them on jffs2 so
 * they are still there whenever someone does look: config (+ a drift-
 * detection hash), software version, FPGA identity, IIO readbacks, bridge
 * counters and peer state, supervisor state, and recent structured events --
 * everything the roadmap named.
 *
 * Deliberately its OWN format, not diagnostic-bundle's: that one embeds the
 * WHOLE 256 KiB status document and the FULL 64 KiB log ring verbatim, sized
 * for a single on-demand pull an operator asked for. Every field here comes
 * from a specific section (fpga, supervisor) or the already-structured
 * events, never raw text, so a handful of these can live on flash at once
 * without approaching what one full diagnostic-bundle already costs alone. */
#define BUNDLE_CAPTURE_COOLDOWN_S 60      // a flapping fault must not spam captures
#define RECOVERY_WINDOW_S         300     // "repeated recovery": N recoveries within this window
#define RECOVERY_WINDOW_THRESHOLD 3
#define MAX_BUNDLES               5
#define MAX_CONF_BYTES            (4 * 1024)
// Applying a config means killing the running appliance and re-exec'ing
// appliance_start.sh, which re-sources bridge.conf, reconfigures the modem
// and AD9363, and only then re-execs into a fresh supervisor. On an already-
// booted board (the only time this ever runs) the IIO devices already exist,
// so that normally takes low single-digit seconds; 20s is a wide multiple of
// that, not a value trimmed to the nominal case, on the same standing rule
// that earned reset_demod's own margin.
#define CONFIG_VERIFY_TIMEOUT_S   20
#define CONFIG_VERIFY_POLL_MS     500

/* FNV-1a: a drift-detection hash for the config, not a security one -- the
 * same reasoning and the same algorithm LoopGuard.hpp uses elsewhere in this
 * project for exactly this "notice if this changed" purpose, nothing more. */
static uint64_t fnv1a(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

static void auto_bundle_json(const struct options *o, const struct logring *logs,
                             const char *trigger, const char *detail, struct sb *out) {
    sb_put(out, "{\"bundle_version\":1,\"trigger\":");
    sb_json_str(out, trigger, strlen(trigger));
    sb_put(out, ",\"detail\":");
    sb_json_str(out, detail ? detail : "", detail ? strlen(detail) : 0);
    sb_putf(out, ",\"captured_at_uptime_s\":%lld", board_uptime_s());

    char rel[64] = "";
    read_small("/mnt/jffs2/sdr-release", rel, sizeof rel);
    sb_put(out, ",\"software_version\":"); sb_json_str(out, rel, strlen(rel));

    struct sb conf; sb_init(&conf);
    int conf_ok = read_tail(o->conf_file, MAX_CONF_BYTES, &conf) && conf.p && conf.n && !conf.overflow;
    sb_put(out, ",\"config\":");
    if (conf_ok) sb_json_str(out, conf.p, conf.n); else sb_put(out, "null");
    sb_put(out, ",\"config_fnv1a\":");
    if (conf_ok) sb_putf(out, "\"%016llx\"", (unsigned long long)fnv1a(conf.p, conf.n));
    else sb_put(out, "null");
    sb_free(&conf);

    // FPGA identity and supervisor state, pulled out of the same status
    // document /api/v1/fpga and /api/v1/supervisor already read a section of
    // -- one fork covers both rather than one each.
    struct sb status; sb_init(&status);
    int status_ok = run_program(o->status_program, MAX_STATUS, &status, IO_TIMEOUT_MS);
    const char *v; size_t vn;
    sb_put(out, ",\"fpga\":");
    if (status_ok && json_member(status.p, status.n, "fpga", &v, &vn)) sb_putn(out, v, vn); else sb_put(out, "null");
    sb_put(out, ",\"supervisor\":");
    if (status_ok && json_member(status.p, status.n, "supervisor", &v, &vn)) sb_putn(out, v, vn); else sb_put(out, "null");
    sb_free(&status);

    sb_put(out, ",\"radio\":");
    { struct sb r; sb_init(&r); radio_json(o, &r); if (r.p) { rtrim(r.p); sb_put(out, r.p); } else sb_put(out, "null"); sb_free(&r); }

    struct sb metrics; sb_init(&metrics);
    int mok = read_tail(o->metrics_file, MAX_METRICS, &metrics) && !blank(metrics.p ? metrics.p : "", metrics.n);
    sb_put(out, ",\"bridge\":");
    char node[24] = "null";
    if (mok) {
        size_t n = metrics.n; while (n && (metrics.p[n-1] == '\n' || metrics.p[n-1] == '\r')) --n;
        sb_putn(out, metrics.p, n);
        json_number_field(metrics.p, metrics.n, "node_id", node, sizeof node);
    } else sb_put(out, "null");
    sb_free(&metrics);

    // Structured events, not the raw log: the distillation is what a
    // snapshot needs, and it costs a small, bounded fraction of what the raw
    // text would.
    sb_put(out, ",\"recent_events\":");
    if (logs && logs->available) events_json(logs->ring, logs->n, node, out); else sb_put(out, "{\"events\":[]}");
    sb_put(out, "}");
}

/* Existing bundle files are named "<sequence>.json"; the sequence is only
 * ever read back from the directory itself (not kept in a variable across
 * calls), so it survives this process restarting without a persisted
 * counter of its own. */
static long next_bundle_number(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 1;
    long max_n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end; long v = strtol(e->d_name, &end, 10);
        if (end != e->d_name && strcmp(end, ".json") == 0 && v > max_n) max_n = v;
    }
    closedir(d);
    return max_n + 1;
}

static void prune_bundles(const char *dir, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    long nums[256]; int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        char *end; long v = strtol(e->d_name, &end, 10);
        if (end != e->d_name && strcmp(end, ".json") == 0) nums[n++] = v;
    }
    closedir(d);
    for (int i = 1; i < n; ++i) {   // n is at most a few dozen in practice; insertion sort is plenty
        long k = nums[i]; int j = i - 1;
        while (j >= 0 && nums[j] > k) { nums[j+1] = nums[j]; --j; }
        nums[j+1] = k;
    }
    for (int i = 0; i < n - keep; ++i) {
        char path[600]; snprintf(path, sizeof path, "%s/%ld.json", dir, nums[i]);
        unlink(path);
    }
}

static long long g_last_bundle_s = -1000000;   // far enough in the past that the first real trigger is never blocked

static void maybe_capture_bundle(const struct options *o, const char *trigger, const char *detail) {
    if (!o->bundle_dir || !*o->bundle_dir) return;
    long long now = board_uptime_s();
    if (now - g_last_bundle_s < o->bundle_cooldown_s) return;
    g_last_bundle_s = now;

    mkdir(o->bundle_dir, 0700);   // ignore EEXIST (and anything else -- the write below fails visibly if this genuinely did not work)
    long num = next_bundle_number(o->bundle_dir);
    struct sb b; sb_init(&b);
    auto_bundle_json(o, g_logs, trigger, detail, &b);
    if (b.p && !b.overflow) {
        char path[600]; snprintf(path, sizeof path, "%s/%ld.json", o->bundle_dir, num);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ssize_t w = write(fd, b.p, b.n); (void)w; close(fd); }
    }
    sb_free(&b);
    prune_bundles(o->bundle_dir, MAX_BUNDLES);
}

/* SUPERVISOR_FAULT and PREFLIGHT_FAIL are each, on their own, exactly the
 * "FAULT" the roadmap names -- one occurrence is enough to capture. DEMOD_
 * RECOVERY is not: a single soft reset is the modem doing its job, and
 * capturing on every one would defeat the cooldown's whole purpose the first
 * time a marginal link recovers a few times in a row. "Repeated recovery" is
 * RECOVERY_WINDOW_THRESHOLD-or-more within RECOVERY_WINDOW_S. */
static void note_event_for_capture(const struct event *e) {
    if (!strcmp(e->code, "SUPERVISOR_FAULT") || !strcmp(e->code, "PREFLIGHT_FAIL")) {
        char detail[200]; size_t n = e->len < sizeof detail - 1 ? e->len : sizeof detail - 1;
        memcpy(detail, e->line, n); detail[n] = 0;
        maybe_capture_bundle(g_opts, e->code, detail);
        return;
    }
    if (strcmp(e->code, "DEMOD_RECOVERY") != 0) return;

    static long long times[RECOVERY_WINDOW_THRESHOLD];
    static int count = 0;
    long long now = board_uptime_s();
    int window_s = (g_opts && g_opts->recovery_window_s > 0) ? g_opts->recovery_window_s : RECOVERY_WINDOW_S;
    // Drop anything that has aged out before adding the new one.
    int kept = 0;
    for (int i = 0; i < count; ++i) if (now - times[i] <= window_s) times[kept++] = times[i];
    count = kept;
    if (count < RECOVERY_WINDOW_THRESHOLD) times[count++] = now;
    else { for (int i = 1; i < RECOVERY_WINDOW_THRESHOLD; ++i) times[i-1] = times[i]; times[RECOVERY_WINDOW_THRESHOLD-1] = now; }
    if (count >= RECOVERY_WINDOW_THRESHOLD) {
        char detail[96];
        snprintf(detail, sizeof detail, "%d demodulator recoveries within %ds", count, window_s);
        maybe_capture_bundle(g_opts, "REPEATED_RECOVERY", detail);
    }
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
    int status_ok = run_program(o->status_program, MAX_STATUS, &status, IO_TIMEOUT_MS);
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

/* The journal on disk is JSONL (one object per line, appended without a
 * wrapping array so a crash mid-write never corrupts anything already
 * written); every other endpoint here returns one JSON document, so this
 * wraps it the same way events_json's own output looks: {"events":[...]}. */
static void fault_history_json(const struct options *o, struct sb *out) {
    sb_put(out, "{\"events\":[");
    if (o->fault_file && *o->fault_file) {
        struct sb raw; sb_init(&raw);
        // Read generously past the enforced bound: journal_append keeps the
        // file at or under fault_max_bytes after every write, so this should
        // always capture the whole file; the generous margin is only a guard
        // against an external actor having grown it some other way.
        if (read_tail(o->fault_file, o->fault_max_bytes * 2, &raw) && raw.p && raw.n) {
            size_t start = 0;
            // A read that came back exactly at its limit may have been cut
            // mid-line; a real line always starts with '{'. Drop a partial
            // leading line rather than hand the caller unparseable JSON.
            if (raw.n >= o->fault_max_bytes * 2 && raw.p[0] != '{') {
                const char *nl = memchr(raw.p, '\n', raw.n);
                start = nl ? (size_t)(nl + 1 - raw.p) : raw.n;
            }
            int first = 1;
            while (start < raw.n) {
                const char *line = raw.p + start;
                const char *nl = memchr(line, '\n', raw.n - start);
                size_t len = nl ? (size_t)(nl - line) : raw.n - start;
                start += len + (nl ? 1 : 0);
                if (len == 0) continue;
                if (!first) sb_put(out, ",");
                first = 0;
                sb_putn(out, line, len);
            }
        }
        sb_free(&raw);
    }
    sb_put(out, "]}");
}

/* A directory listing that happens to be readable as JSON, not a browsable
 * one -- filenames are our own "<N>.json" sequence numbers, and each list
 * entry is a small summary pulled out of the file's own trigger/detail/
 * captured_at fields, not the whole bundle (fetch it by number for that). */
static void bundles_list_json(const struct options *o, struct sb *out) {
    sb_put(out, "{\"bundles\":[");
    if (!o->bundle_dir || !*o->bundle_dir) { sb_put(out, "]}"); return; }
    DIR *d = opendir(o->bundle_dir);
    if (!d) { sb_put(out, "]}"); return; }
    long nums[MAX_BUNDLES]; int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_BUNDLES) {
        char *end; long v = strtol(e->d_name, &end, 10);
        if (end != e->d_name && strcmp(end, ".json") == 0) nums[n++] = v;
    }
    closedir(d);
    for (int i = 1; i < n; ++i) { long k = nums[i]; int j = i - 1; while (j >= 0 && nums[j] > k) { nums[j+1] = nums[j]; --j; } nums[j+1] = k; }
    for (int i = 0; i < n; ++i) {
        char path[600]; snprintf(path, sizeof path, "%s/%ld.json", o->bundle_dir, nums[i]);
        struct sb one; sb_init(&one);
        if (!read_tail(path, MAX_BODY, &one) || !one.p) { sb_free(&one); continue; }
        const char *v; size_t vn;
        if (i) sb_put(out, ",");
        sb_putf(out, "{\"id\":%ld,\"trigger\":", nums[i]);
        if (json_member(one.p, one.n, "trigger", &v, &vn)) sb_putn(out, v, vn); else sb_put(out, "null");
        sb_put(out, ",\"detail\":");
        if (json_member(one.p, one.n, "detail", &v, &vn)) sb_putn(out, v, vn); else sb_put(out, "null");
        sb_put(out, ",\"captured_at_uptime_s\":");
        if (json_member(one.p, one.n, "captured_at_uptime_s", &v, &vn)) sb_putn(out, v, vn); else sb_put(out, "null");
        sb_put(out, "}");
        sb_free(&one);
    }
    sb_put(out, "]}");
}

/* Serve one bundle by id, or the most recently captured one. `id_str` is
 * whatever followed "/api/v1/bundles/" in the request path -- untrusted, and
 * validated as a plain non-negative integer (or the literal "latest") before
 * it ever reaches a filename, never passed through. */
static void reply_bundle(int fd, const struct options *o, const char *id_str) {
    if (!o->bundle_dir || !*o->bundle_dir) { unavailable(fd, "bundles"); return; }
    long id;
    if (!strcmp(id_str, "latest")) {
        id = next_bundle_number(o->bundle_dir) - 1;
        if (id < 1) { unavailable(fd, "bundles"); return; }
    } else {
        if (!*id_str) { reply_json(fd, 404, "Not Found", "{\"error\":\"not found\"}\n"); return; }
        char *end; id = strtol(id_str, &end, 10);
        if (*end || id < 1) { reply_json(fd, 404, "Not Found", "{\"error\":\"not found\"}\n"); return; }
    }
    char path[600]; snprintf(path, sizeof path, "%s/%ld.json", o->bundle_dir, id);
    struct stat st;
    if (stat(path, &st) != 0) { reply_json(fd, 404, "Not Found", "{\"error\":\"not found\"}\n"); return; }
    struct sb b; sb_init(&b);
    if (!read_tail(path, MAX_BODY, &b) || !b.p || !b.n) { sb_free(&b); unavailable(fd, "bundle"); return; }
    reply_sb(fd, &b, "application/json");
    sb_free(&b);
}

/* ── Safe remote control ─────────────────────────────────────────────────
 * Every action below is a fixed, hand-written operation on a fixed process
 * identity or a fixed script -- never a caller-supplied command or path --
 * so this cannot become a general remote shell no matter what a request
 * sends. `<action>` itself is matched against exactly five literal names;
 * anything else falls through to 404, same as any other unknown route. */

/* Bounded process identification, matched the same way
 * appliance_supervise.sh's own kill_bridges() does: by resolved /exe
 * identity for the bridge and its IIO helpers (a command-line pattern is
 * also present in the argv of whatever searches for it, which is exactly
 * the kind of self-match that script's own comment warns about), and by
 * /cmdline substring for the supervisor, which has no distinctive /exe.
 * PROC_SUPERVISOR matches EITHER appliance_supervise.sh or
 * appliance_start.sh: appliance_start.sh execs into the supervisor only
 * after its own preflight (waiting on IIO devices, validating bridge.conf)
 * succeeds, so a kill aimed at "the supervisor" while that preflight is
 * still running would miss it under only the narrower match -- the process
 * would survive, finish bringing itself up moments later, and exec into a
 * brand new supervisor no kill ever targeted. Config-apply's rollback
 * depends on this: it kills, waits, and re-launches, and a surviving
 * straggler from the PREVIOUS launch would race the new one.
 * PROC_BRIDGE_ONLY exists separately from PROC_BRIDGE_AND_HELPERS because
 * clear_counters signals SIGUSR1 -- fine for sdr_bridge, which installs a
 * handler for it, but SIGUSR1's default disposition is to terminate a
 * process that does not, and iio_readdev/iio_writedev do not. Passing
 * sig=0 sends nothing (POSIX kill(2)) and just tests whether a match
 * exists, which is how restart_bridge reports supervisor presence without
 * disturbing it. */
enum proc_family { PROC_BRIDGE_ONLY, PROC_BRIDGE_AND_HELPERS, PROC_SUPERVISOR };

static int signal_by_identity(enum proc_family fam, int sig) {
    int hit = 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[288], buf[256];
        int matched = 0;
        if (fam == PROC_SUPERVISOR) {
            snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
            FILE *f = fopen(path, "r");
            if (f) {
                size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f);
                buf[got] = 0;
                for (size_t i = 0; i < got; ++i) if (buf[i] == 0) buf[i] = ' ';
                if (strstr(buf, "appliance_supervise") || strstr(buf, "appliance_start")) matched = 1;
            }
        } else {
            snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
            ssize_t n = readlink(path, buf, sizeof buf - 1);
            if (n > 0) {
                buf[n] = 0;
                if (strstr(buf, "/sdr_bridge")) matched = 1;
                else if (fam == PROC_BRIDGE_AND_HELPERS &&
                         (strstr(buf, "/iio_readdev") || strstr(buf, "/iio_writedev")))
                    matched = 1;
            }
        }
        if (matched) { kill((pid_t)atol(e->d_name), sig); ++hit; }
    }
    closedir(d);
    return hit;
}

/* Bounded "key=value" lookup over a query string already split off the path
 * by the caller. No form-decoding: the confirm gate only ever needs one
 * exact literal match, so decoding would be attack surface for no benefit. */
static int query_has(const char *query, const char *key, const char *value) {
    if (!query || !*query) return 0;
    size_t klen = strlen(key), vlen = strlen(value);
    for (const char *p = query; *p; ) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen == klen + 1 + vlen && !strncmp(p, key, klen) && p[klen] == '=' &&
            !strncmp(p + klen + 1, value, vlen))
            return 1;
        p += seglen; if (*p == '&') ++p;
    }
    return 0;
}

static void reply_control(int fd, const struct options *o, const char *action, const char *query) {
    if (!query_has(query, "confirm", "yes")) {
        reply_json(fd, 400, "Bad Request", "{\"error\":\"control actions require ?confirm=yes\"}\n");
        return;
    }
    char body[512];
    if (!strcmp(action, "restart_bridge")) {
        push_synth("warning", "control", "BRIDGE_RESTART_REQUESTED", "operator requested restart_bridge");
        int killed = signal_by_identity(PROC_BRIDGE_AND_HELPERS, SIGKILL);
        int supervised = signal_by_identity(PROC_SUPERVISOR, 0) > 0;
        snprintf(body, sizeof body, "{\"action\":\"restart_bridge\",\"killed\":%d,\"supervisor_present\":%s}\n",
                 killed, supervised ? "true" : "false");
        reply_json(fd, 200, "OK", body);
    } else if (!strcmp(action, "clear_counters")) {
        push_synth("info", "control", "COUNTERS_CLEARED", "operator requested clear_counters");
        int signalled = signal_by_identity(PROC_BRIDGE_ONLY, SIGUSR1);
        snprintf(body, sizeof body, "{\"action\":\"clear_counters\",\"signalled\":%d}\n", signalled);
        reply_json(fd, 200, "OK", body);
    } else if (!strcmp(action, "reset_demod")) {
        push_synth("warning", "control", "DEMOD_RESET_REQUESTED", "operator requested reset_demod");
        struct sb b; sb_init(&b);
        int ok = run_program(o->demod_reset_script, MAX_STATUS, &b, DEMOD_RESET_TIMEOUT_MS);
        struct sb out; sb_init(&out);
        sb_putf(&out, "{\"action\":\"reset_demod\",\"ok\":%s,\"output\":", ok ? "true" : "false");
        sb_json_str(&out, b.p ? b.p : "", b.n);
        sb_put(&out, "}\n");
        reply_sb(fd, &out, "application/json");
        sb_free(&out); sb_free(&b);
    } else if (!strcmp(action, "enter_safe_mode")) {
        push_synth("error", "control", "SAFE_MODE_REQUESTED", "operator requested enter_safe_mode");
        // Supervisor first: killing it before the bridge means it cannot
        // notice the bridge is gone and bring it right back up underneath
        // this request. Order the other way and safe mode could observe
        // "killed" while a fresh bridge is already starting.
        int nsup = signal_by_identity(PROC_SUPERVISOR, SIGKILL);
        int nbridge = signal_by_identity(PROC_BRIDGE_AND_HELPERS, SIGKILL);
        snprintf(body, sizeof body, "{\"action\":\"enter_safe_mode\",\"supervisor_killed\":%d,\"bridge_killed\":%d}\n",
                 nsup, nbridge);
        reply_json(fd, 200, "OK", body);
    } else if (!strcmp(action, "restart_appliance")) {
        push_synth("error", "control", "APPLIANCE_REBOOT_REQUESTED", "operator requested restart_appliance");
        // Reply before forking: the ack needs to be on the wire before the
        // board actually goes down. Plain `reboot` is a documented no-op on
        // this firmware (see project memory); `-f` is required, and this
        // execs it directly, never through a shell.
        reply_json(fd, 200, "OK", "{\"action\":\"restart_appliance\",\"ok\":true}\n");
        pid_t pid = fork();
        if (pid == 0) { setsid(); execl("/sbin/reboot", "reboot", "-f", (char *)NULL); _exit(127); }
    } else {
        reply_json(fd, 404, "Not Found", "{\"error\":\"unknown control action\"}\n");
    }
}

/* ── Safe configuration API ──────────────────────────────────────────────
 * GET config -> validate candidate -> apply -> verify -> rollback.
 * bridge.conf is NEVER overwritten with anything that has not already
 * passed config_schema.sh's own validation, run against a candidate file on
 * disk, not the live one -- an invalid candidate never touches bridge.conf
 * at all, and the request fails with the validator's own error text.
 *
 * A validated candidate still is not enough: bridge.conf only takes effect
 * when appliance_start.sh re-sources it, which means killing the running
 * appliance and re-launching that script from scratch (a plain
 * restart_bridge is not enough -- see PROC_SUPERVISOR's comment above:
 * appliance_supervise.sh runs with BRIDGE_ARGS fixed at the moment
 * appliance_start.sh execs into it, so restarting only the bridge process
 * restarts it with the OLD arguments). "Verify" polls for a bridge process
 * AND for the AD9363 actually converging on the new requested values,
 * because a config that only validates syntactically is not the same as
 * one the radio actually took. "Rollback" restores the one prior config
 * this endpoint itself backed up before touching anything, and re-applies
 * that the same way -- best-effort, not re-verified in turn, so one bad
 * apply cannot recurse into an unbounded chain of retries. */

/* Kill whatever appliance is currently running -- by IDENTITY, never a
 * command-line pattern, same reasoning as every other kill in this file --
 * and launch a fresh appliance_start.sh, detached, so it re-sources
 * whatever bridge.conf is on disk AT THE MOMENT IT RUNS. Used both to apply
 * a newly-installed config and, unchanged, to roll one back: the only
 * difference between the two calls is which file is on disk first. */
static void relaunch_appliance(const struct options *o) {
    signal_by_identity(PROC_SUPERVISOR, SIGKILL);
    signal_by_identity(PROC_BRIDGE_AND_HELPERS, SIGKILL);
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(o->start_script, o->start_script, (char *)NULL); _exit(127); }
}

/* Poll for the appliance actually coming back up on the config now on disk,
 * within a bounded window -- see CONFIG_VERIFY_TIMEOUT_S's comment for why
 * that bound is sized the way it is. */
static int wait_for_bridge_and_radio(const struct options *o) {
    long long deadline = now_ms() + (long long)o->config_verify_timeout_s * 1000;
    do {
        if (signal_by_identity(PROC_BRIDGE_ONLY, 0) > 0 && radio_converged(o)) return 1;
        usleep(CONFIG_VERIFY_POLL_MS * 1000);
    } while (now_ms() < deadline);
    return 0;
}

/* This is the one route in this file that can block for a long time (up to
 * CONFIG_VERIFY_TIMEOUT_S) before replying, and deliberately does not fork
 * the way /api/v1/stream does: applying a config is a rare, operator-
 * initiated action that already kills the running appliance, and the
 * caller explicitly asked for apply-verify-rollback as ONE outcome, not a
 * "started" acknowledgment they would have to go poll for separately. The
 * cost is that this daemon serves no other request for the duration -- an
 * accepted tradeoff for an action this infrequent and already this
 * disruptive, not an oversight. */
static void reply_config_apply(int fd, const struct options *o, const char *query, const char *body, size_t body_len) {
    if (!query_has(query, "confirm", "yes")) {
        reply_json(fd, 400, "Bad Request", "{\"error\":\"config apply requires ?confirm=yes\"}\n");
        return;
    }
    if (body_len == 0) {
        reply_json(fd, 400, "Bad Request", "{\"error\":\"empty candidate config\"}\n");
        return;
    }

    char candidate[600]; snprintf(candidate, sizeof candidate, "%s.candidate", o->conf_file);
    int cfd = open(candidate, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (cfd < 0) { reply_json(fd, 500, "Internal Server Error", "{\"error\":\"cannot write candidate\"}\n"); return; }
    ssize_t w = write(cfd, body, body_len);
    close(cfd);
    if (w < 0 || (size_t)w != body_len) {
        unlink(candidate);
        reply_json(fd, 500, "Internal Server Error", "{\"error\":\"candidate write incomplete\"}\n");
        return;
    }

    // Validate the CANDIDATE on disk, not the live file -- bridge.conf is
    // not touched at all on this path.
    struct sb verr; sb_init(&verr);
    char *argv[] = { (char *)o->config_schema_script, (char *)"validate", candidate, NULL };
    int valid = run_program_ex(o->config_schema_script, argv, MAX_STATUS, &verr, IO_TIMEOUT_MS, 1);
    if (!valid) {
        unlink(candidate);
        push_synth("warning", "control", "CONFIG_REJECTED", "candidate bridge.conf failed validation");
        struct sb out; sb_init(&out);
        sb_put(&out, "{\"action\":\"apply_config\",\"validated\":false,\"applied\":false,"
                     "\"verified\":false,\"rolled_back\":false,\"validation_output\":");
        sb_json_str(&out, verr.p ? verr.p : "", verr.n);
        sb_put(&out, "}\n");
        reply_sb(fd, &out, "application/json");
        sb_free(&out); sb_free(&verr);
        return;
    }
    sb_free(&verr);

    // Back up whatever is currently in effect BEFORE it is touched: if the
    // new config fails to verify, this backup is the only way back.
    char backup[600]; snprintf(backup, sizeof backup, "%s.prev", o->conf_file);
    struct sb cur; sb_init(&cur);
    int have_backup = read_tail(o->conf_file, MAX_CONF_BYTES, &cur) && cur.p && cur.n;
    if (have_backup) {
        int bfd = open(backup, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (bfd >= 0) { ssize_t bw = write(bfd, cur.p, cur.n); (void)bw; close(bfd); }
    }
    sb_free(&cur);

    // Commit point: rename is atomic on the same filesystem, sync on both
    // sides so the commit survives a power loss, same shape as
    // config_schema.sh's own write_atomic.
    sync();
    if (rename(candidate, o->conf_file) != 0) {
        unlink(candidate);
        reply_json(fd, 500, "Internal Server Error", "{\"error\":\"could not install candidate\"}\n");
        return;
    }
    sync();

    push_synth("warning", "control", "CONFIG_APPLIED",
               "operator applied a new bridge.conf; restarting the appliance to pick it up");
    relaunch_appliance(o);
    int verified = wait_for_bridge_and_radio(o);

    int rolled_back = 0;
    if (!verified && have_backup) {
        sync();
        rename(backup, o->conf_file);
        sync();
        push_synth("error", "control", "CONFIG_ROLLBACK",
                   "new bridge.conf did not verify within the timeout; restored the previous configuration");
        relaunch_appliance(o);
        rolled_back = 1;
    }

    char body_out[400];
    snprintf(body_out, sizeof body_out,
             "{\"action\":\"apply_config\",\"validated\":true,\"applied\":true,\"verified\":%s,\"rolled_back\":%s}\n",
             verified ? "true" : "false", rolled_back ? "true" : "false");
    reply_json(fd, 200, "OK", body_out);
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
    // Split off a query string once, here, so every route below matches on
    // the bare path regardless of whether one is present. Only the control
    // actions read `query` (for confirm=yes); every existing GET route
    // ignores it, which just means a stray "?anything" no longer 404s them.
    char *query = strchr(path, '?');
    if (query) *query++ = 0; else query = path + strlen(path);
    char auth[600] = ""; char *body_start = hdr_end + 4; long content_length = -1;
    for (char *p = line_end ? line_end + 2 : req + strlen(req); p && *p; ) {
        char *e = strstr(p, "\r\n"); if (e) *e = 0;
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            if (strcasecmp(ltrim(p), "authorization") == 0) {
                strncpy(auth, ltrim(colon + 1), sizeof auth - 1); rtrim(auth);
            } else if (strcasecmp(ltrim(p), "content-length") == 0) {
                content_length = strtol(ltrim(colon + 1), NULL, 10);
            }
        }
        p = e ? e + 2 : NULL;
    }
    char expected[300]; snprintf(expected, sizeof expected, "Bearer %s", token);
    if (!constant_time_equal(auth, expected)) { reply_json(fd, 401, "Unauthorized", "{\"error\":\"unauthorized\"}\n"); return; }
    // Every route is GET-only except the control actions (POST, state-
    // changing) and /api/v1/config, which accepts both: GET reads the
    // current section, POST installs a validated candidate. A GET must
    // never be able to trigger a mutation just by being requested (a
    // prefetch, a browser history revisit, a monitoring crawler).
    static const char CONTROL_PREFIX[] = "/api/v1/control/";
    int is_control = !strncmp(path, CONTROL_PREFIX, sizeof CONTROL_PREFIX - 1);
    int is_config_path = !strcmp(path, "/api/v1/config");
    int method_ok = is_control ? !strcmp(method, "POST")
                  : is_config_path ? (!strcmp(method, "GET") || !strcmp(method, "POST"))
                  : !strcmp(method, "GET");
    if (!method_ok) {
        reply_json(fd, 405, "Method Not Allowed",
                   is_control ? "{\"error\":\"control actions require POST\"}\n"
                 : is_config_path ? "{\"error\":\"config accepts GET or POST\"}\n"
                                  : "{\"error\":\"read-only API\"}\n");
        return;
    }
    // Only one route ever has a body. Read it here, once, rather than
    // unconditionally for every request: a bodyless GET is the overwhelming
    // majority of traffic, and the initial read loop above already stops at
    // end-of-headers, so a POST's body may not have fully arrived yet.
    char *body = NULL; size_t body_len = 0;
    if (is_config_path && !strcmp(method, "POST")) {
        if (content_length < 0 || (size_t)content_length > MAX_CONF_BYTES) {
            reply_json(fd, 413, "Payload Too Large", "{\"error\":\"candidate config missing or too large\"}\n");
            return;
        }
        size_t have = n - (size_t)(body_start - req);
        long long deadline = now_ms() + IO_TIMEOUT_MS;
        while (have < (size_t)content_length && n < MAX_REQUEST && now_ms() < deadline) {
            ssize_t r = recv(fd, req + n, MAX_REQUEST - n, 0);
            if (r > 0) { n += (size_t)r; have += (size_t)r; }
            else if (r < 0 && errno == EINTR) continue;
            else break;
        }
        if (have < (size_t)content_length) {
            reply_json(fd, 400, "Bad Request", "{\"error\":\"candidate config body incomplete\"}\n");
            return;
        }
        body = body_start; body_len = (size_t)content_length;
    }

    if (strcmp(path, "/api/v1/health") == 0) {
        char body[128]; snprintf(body, sizeof body, "{\"status\":\"ok\",\"service\":\"sdr-agent\",\"version\":%d}\n", SERVICE_VERSION);
        reply_json(fd, 200, "OK", body);
    } else if (strcmp(path, "/api/v1/status") == 0) {
        struct sb b; sb_init(&b);
        if (!run_program(o->status_program, MAX_STATUS, &b, IO_TIMEOUT_MS)) unavailable(fd, "status");
        else reply_sb(fd, &b, "application/json");
        sb_free(&b);
    } else if (strcmp(path, "/api/v1/fpga") == 0 || strcmp(path, "/api/v1/ethernet") == 0 ||
               strcmp(path, "/api/v1/supervisor") == 0 || strcmp(path, "/api/v1/modem") == 0 ||
               (is_config_path && !strcmp(method, "GET")) || strcmp(path, "/api/v1/progress") == 0) {
        struct sb b; sb_init(&b);
        int ok = run_program(o->status_program, MAX_STATUS, &b, IO_TIMEOUT_MS);
        reply_section(fd, &b, ok, path + 8, path + 8);
        sb_free(&b);
    } else if (is_config_path) {   // the remaining case, method already validated above: POST
        reply_config_apply(fd, o, query, body, body_len);
    } else if (strcmp(path, "/api/v1/config/raw") == 0) {
        struct sb b; sb_init(&b);
        if (!read_tail(o->conf_file, MAX_CONF_BYTES, &b) || !b.p || !b.n) { unavailable(fd, "config"); sb_free(&b); return; }
        struct sb out; sb_init(&out);
        sb_put(&out, "{\"config\":"); sb_json_str(&out, b.p, b.n); sb_put(&out, "}\n");
        reply_sb(fd, &out, "application/json");
        sb_free(&out); sb_free(&b);
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
    } else if (strcmp(path, "/api/v1/fault-history") == 0) {
        struct sb b; sb_init(&b); fault_history_json(o, &b); reply_sb(fd, &b, "application/json"); sb_free(&b);
    } else if (strcmp(path, "/api/v1/bundles") == 0) {
        struct sb b; sb_init(&b); bundles_list_json(o, &b); reply_sb(fd, &b, "application/json"); sb_free(&b);
    } else if (!strncmp(path, "/api/v1/bundles/", 16)) {
        reply_bundle(fd, o, path + 16);
    } else if (is_control) {
        reply_control(fd, o, path + (sizeof CONTROL_PREFIX - 1), query);
    } else {
        reply_json(fd, 404, "Not Found", "{\"error\":\"not found\"}\n");
    }
}

static int usage(void) {
    fprintf(stderr, "usage: sdr-agent [--bind IP] [--port N] [--token-file PATH] [--status-program PATH]\n"
                    "                 [--metrics-file PATH] [--log-file PATH] [--conf PATH] [--iio-dir PATH]\n"
                    "                 [--fault-file PATH] [--bundle-dir PATH] [--demod-reset-script PATH]\n"
                    "                 [--config-schema-script PATH] [--start-script PATH]\n"
                    "                 [--rf-loss-threshold-s N] [--fault-max-bytes N]\n"
                    "                 [--bundle-cooldown-s N] [--recovery-window-s N]\n"
                    "                 [--config-verify-timeout-s N]\n");
    return 2;
}

int main(int argc, char **argv) {
    struct options o = {
        "127.0.0.1", "/mnt/jffs2/agent.token", "/mnt/jffs2/appliance_status.sh",
        "/tmp/bridge_stats.json", "/tmp/appliance.log", "/mnt/jffs2/bridge.conf",
        "/sys/bus/iio/devices", "/mnt/jffs2/fault_history.jsonl", "/mnt/jffs2/bundles",
        "/mnt/jffs2/tools/reset_demod.sh",
        "/mnt/jffs2/config_schema.sh", "/mnt/jffs2/appliance_start.sh",
        8088, RF_LOSS_THRESHOLD_S, FAULT_JOURNAL_MAX_BYTES,
        BUNDLE_CAPTURE_COOLDOWN_S, RECOVERY_WINDOW_S, CONFIG_VERIFY_TIMEOUT_S
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
        else if (!strcmp(a, "--fault-file")) o.fault_file = v;
        else if (!strcmp(a, "--bundle-dir")) o.bundle_dir = v;
        else if (!strcmp(a, "--demod-reset-script")) o.demod_reset_script = v;
        else if (!strcmp(a, "--config-schema-script")) o.config_schema_script = v;
        else if (!strcmp(a, "--start-script")) o.start_script = v;
        // Production has no reason to change any of these from their
        // defaults -- they exist so a test can see RF_LOSS fire in seconds
        // instead of the real 120s threshold, see the journal actually get
        // truncated without first writing 32 KiB of real entries to it, and
        // see a bundle get captured without waiting 60s between triggers or
        // manufacturing 300s of real recovery history.
        else if (!strcmp(a, "--rf-loss-threshold-s")) o.rf_loss_threshold_s = atoi(v);
        else if (!strcmp(a, "--fault-max-bytes")) o.fault_max_bytes = (size_t)atol(v);
        else if (!strcmp(a, "--bundle-cooldown-s")) o.bundle_cooldown_s = atoi(v);
        else if (!strcmp(a, "--recovery-window-s")) o.recovery_window_s = atoi(v);
        else if (!strcmp(a, "--config-verify-timeout-s")) o.config_verify_timeout_s = atoi(v);
        else return usage();
    }
    if (o.port < 1 || o.port > 65535) { fprintf(stderr, "invalid port\n"); return 2; }
    if (o.rf_loss_threshold_s < 1) { fprintf(stderr, "invalid --rf-loss-threshold-s\n"); return 2; }
    if (o.fault_max_bytes < 1024) { fprintf(stderr, "invalid --fault-max-bytes\n"); return 2; }
    if (o.bundle_cooldown_s < 0) { fprintf(stderr, "invalid --bundle-cooldown-s\n"); return 2; }
    if (o.recovery_window_s < 1) { fprintf(stderr, "invalid --recovery-window-s\n"); return 2; }
    if (o.config_verify_timeout_s < 1) { fprintf(stderr, "invalid --config-verify-timeout-s\n"); return 2; }
    g_opts = &o;
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
    printf("sdr-agent: http://%s:%d (authenticated)\n", o.bind_addr, o.port);
    fflush(stdout);

    static struct logring logs; memset(&logs, 0, sizeof logs); logs.path = o.log_file;
    g_logs = &logs;
    static struct fault_scanner fscan; memset(&fscan, 0, sizeof fscan); fscan.path = o.log_file;
    ring_refresh(&logs);
    while (running) {
        // Scan for new log-classifiable events BEFORE LogRing's own refresh,
        // which can truncate the source at MAX_SOURCE_LOG: on the one
        // iteration that happens on, this ordering is what lets the scanner
        // see the last pre-truncation bytes at all.
        journal_scan(&fscan);
        ring_refresh(&logs);
        reap_streams();
        check_counters(&o);
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
