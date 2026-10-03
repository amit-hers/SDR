// rx_telemetry.c -- high-rate ring-buffer sampler for the demod core's
// diagnostic registers, triggered to freeze a window around a frame-decode
// stall (an sdr_bridge RECOVERY event).
//
// WHY A NEW TOOL, NOT MORE SHELL+devmem. The earlier shell-script poller
// forked a fresh `devmem` process per register per sample, which on this
// board's CPU cost enough jitter (50-120ms between samples, unevenly) that
// the delta/wraparound arithmetic built on top of it produced physically
// impossible results (computed AGC gain exceeding the hardware's own 4.0
// clamp). This tool opens /dev/mem ONCE, mmaps the demod register window,
// and reads all four registers with plain memory loads in a tight loop --
// no fork/exec per sample, so the interval is as even as this CPU can make
// it and the timestamps are trustworthy.
//
// WHAT IT DOES NOT DO. It never writes to soft_reset or demod_enabled, and
// it runs independently of sdr_bridge -- it does not touch the live
// datapath or change modem behavior. It is read-only observability, nothing
// else.
//
// TRIGGER. The FPGA-side registers (lock_count, mu_clamped, the two sums)
// give no signal on their own that a stall happened -- that is the whole
// finding from the earlier investigation: they keep moving normally right
// through every stall. The only thing that actually knows a stall occurred
// is sdr_bridge's own `recoveries` counter, already published in
// /tmp/bridge_stats.json. So this tool samples registers at high rate into
// a ring buffer continuously, and at a much slower rate (every ~20 samples)
// cheaply checks that one field; when it increments, the ring buffer -- which
// already holds dense history from well before the stall was even detected,
// because detection itself takes 2 full stats intervals -- is frozen and
// dumped to disk, plus a short further capture so the dump also covers the
// recovery itself.
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define REG_BASE   0x43C00000UL
#define REG_LEN    0x1000
#define OFF_LOCK   0x18
#define OFF_MU     0x30
#define OFF_GAIN   0x38
#define OFF_ERR    0x40

#define RING_SIZE       8192   /* samples */
#define STATS_CHECK_EVERY 20   /* read bridge_stats.json this often (in samples) */
#define POST_EVENT_US   1500000 /* keep sampling this long after a trigger before dumping */

typedef struct {
    double   t;
    uint32_t lock, mu, gainsum, errsum;
} Sample;

static Sample ring[RING_SIZE];
static int ring_pos = 0, ring_full = 0;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Minimal, dependency-free field pull: bridge_stats.json's own writer
// (sdr_bridge.cpp) explicitly documents the format as fixed and intended to
// be read this way, not re-parsed by pattern -- "the numbers are published
// directly here and nothing needs to parse prose" -- so a plain substring
// search for a known, stable key is exactly the supported access pattern,
// not a fragile scrape.
static long read_recoveries(void) {
    FILE* f = fopen("/tmp/bridge_stats.json", "r");
    if (!f) return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* p = strstr(buf, "\"recoveries\":");
    if (!p) return -1;
    return strtol(p + strlen("\"recoveries\":"), NULL, 10);
}

static void dump_ring(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { perror("rx_telemetry: fopen dump"); return; }
    int start = ring_full ? ring_pos : 0;
    int count = ring_full ? RING_SIZE : ring_pos;
    for (int i = 0; i < count; ++i) {
        Sample* s = &ring[(start + i) % RING_SIZE];
        fprintf(f, "%.6f %u %u %u %u\n", s->t, s->lock, s->mu, s->gainsum, s->errsum);
    }
    fclose(f);
    fprintf(stderr, "rx_telemetry: dumped %d samples to %s\n", count, path);
}

int main(int argc, char** argv) {
    const char* out_prefix = argc > 1 ? argv[1] : "/tmp/rx_telemetry";
    long rate_hz = argc > 2 ? strtol(argv[2], NULL, 10) : 150;
    if (rate_hz < 1) rate_hz = 150;
    const long period_us = 1000000L / rate_hz;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("rx_telemetry: open /dev/mem"); return 1; }
    volatile uint8_t* base = (volatile uint8_t*)mmap(
        NULL, REG_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, REG_BASE);
    if (base == MAP_FAILED) { perror("rx_telemetry: mmap"); return 1; }

    long last_recoveries = read_recoveries();
    int  dump_count = 0;
    int  check_counter = 0;

    fprintf(stderr, "rx_telemetry: started at ~%ldHz, baseline recoveries=%ld, prefix=%s\n",
            rate_hz, last_recoveries, out_prefix);

    for (;;) {
        Sample s;
        s.t       = now_sec();
        s.lock    = *(volatile uint32_t*)(base + OFF_LOCK);
        s.mu      = *(volatile uint32_t*)(base + OFF_MU);
        s.gainsum = *(volatile uint32_t*)(base + OFF_GAIN);
        s.errsum  = *(volatile uint32_t*)(base + OFF_ERR);
        ring[ring_pos] = s;
        ring_pos = (ring_pos + 1) % RING_SIZE;
        if (ring_pos == 0) ring_full = 1;

        if (++check_counter >= STATS_CHECK_EVERY) {
            check_counter = 0;
            long r = read_recoveries();
            if (r > last_recoveries) {
                fprintf(stderr, "rx_telemetry: RECOVERY detected (recoveries %ld -> %ld)\n",
                        last_recoveries, r);
                last_recoveries = r;
                usleep(POST_EVENT_US); // keep sampling so the dump covers the recovery too
                char path[256];
                snprintf(path, sizeof path, "%s_%d.log", out_prefix, dump_count++);
                dump_ring(path);
            } else if (r >= 0) {
                last_recoveries = r;
            }
        }

        usleep((useconds_t)period_us);
    }
    return 0;
}
