#include <iio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>

#define SAMPLE_RATE  2500000
#define BUF_SIZE     (32 * 1024)

typedef struct { float freq; float db; } Result;

int cmp_desc(const void *a, const void *b) {
    float da = ((Result*)a)->db, db = ((Result*)b)->db;
    return (da < db) - (da > db);
}

int main() {
    struct iio_context *ctx = iio_create_network_context("192.168.2.1");
    if (!ctx) { fprintf(stderr, "Cannot connect\n"); return 1; }

    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *rx  = iio_context_find_device(ctx, "cf-ad9361-lpc");

    struct iio_channel *lo   = iio_device_find_channel(phy, "altvoltage0", true);
    struct iio_channel *rxch = iio_device_find_channel(phy, "voltage0", false);
    iio_channel_attr_write_longlong(rxch, "sampling_frequency", SAMPLE_RATE);
    iio_channel_attr_write_longlong(rxch, "rf_bandwidth", 200000);
    iio_channel_attr_write(rxch, "gain_control_mode", "fast_attack");

    iio_channel_enable(iio_device_find_channel(rx, "voltage0", false));
    iio_channel_enable(iio_device_find_channel(rx, "voltage1", false));

    struct iio_buffer *buf = iio_device_create_buffer(rx, BUF_SIZE, false);

    /* Scan 87.5 to 108.0 MHz in 0.1 MHz steps */
    int n_freqs = 206;
    Result results[206];

    fprintf(stderr, "Scanning FM band 87.5 - 108.0 MHz...\n");

    for (int i = 0; i < n_freqs; i++) {
        float freq_mhz = 87.5f + i * 0.1f;
        long long freq_hz = (long long)(freq_mhz * 1e6);

        iio_channel_attr_write_longlong(lo, "frequency", freq_hz);
        usleep(80000); /* 80ms settle time */

        iio_buffer_refill(buf);

        int16_t *p   = (int16_t *)iio_buffer_start(buf);
        int16_t *end = (int16_t *)iio_buffer_end(buf);
        double power = 0;
        int count = 0;
        while (p < end) {
            double iv = *p++ / 32768.0;
            double qv = *p++ / 32768.0;
            power += iv*iv + qv*qv;
            count++;
        }
        power /= count;
        float db = (float)(10.0 * log10(power + 1e-12));

        results[i].freq = freq_mhz;
        results[i].db   = db;

        fprintf(stderr, "\r%.1f MHz  [%3d/%d]", freq_mhz, i+1, n_freqs);
        fflush(stderr);
    }

    qsort(results, n_freqs, sizeof(Result), cmp_desc);

    fprintf(stderr, "\n\n");
    printf("%-14s %-12s %s\n", "Freq (MHz)", "Power (dB)", "Signal strength");
    printf("%.50s\n", "--------------------------------------------------");

    for (int i = 0; i < 15; i++) {
        int bar = (int)((results[i].db + 60) / 2);
        if (bar < 0) bar = 0;
        if (bar > 25) bar = 25;
        printf("%-14.1f %-12.1f ", results[i].freq, results[i].db);
        for (int b = 0; b < bar; b++) printf("█");
        printf("\n");
    }

    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    return 0;
}
