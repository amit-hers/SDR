#include <iio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>

/*
 * FM Radio receiver
 * Usage: ./fm_radio [frequency_MHz]
 * Pipe audio: ./fm_radio 98.5 | aplay -r 48000 -f S16_LE -c 1
 */

#define SAMPLE_RATE     2500000     /* 2.5 MSPS IQ input */
#define AUDIO_RATE      48000       /* output audio rate */
#define DECIM           52          /* 2500000 / 52 = 48076 ~48kHz */
#define BUF_SIZE        (256 * 1024)
#define GAIN_DB         40
#define BANDWIDTH       200000      /* 200 kHz RF bandwidth for FM */

static volatile bool stop = false;
static void handle_sig(int sig) { stop = true; }

/* Simple low-pass FIR coefficients (51-tap, cutoff ~15kHz at 48kHz) */
#define FIR_LEN 9
static const float fir[FIR_LEN] = {
    0.0564, 0.0955, 0.1399, 0.1737, 0.1890,
    0.1737, 0.1399, 0.0955, 0.0564
};

/* Spectrum display - ASCII waterfall in terminal */
static void print_spectrum(float *power_db, int bins, float center_mhz, float bw_mhz) {
    const int WIDTH = 60;
    fprintf(stderr, "\033[2J\033[H"); /* clear screen */
    fprintf(stderr, "  Spectrum: %.3f MHz  BW: %.1f MHz\n", center_mhz, bw_mhz);
    fprintf(stderr, "  %*.1f MHz %*.1f MHz\n", WIDTH/2, center_mhz - bw_mhz/2,
                                                WIDTH/2, center_mhz + bw_mhz/2);

    /* Find max for normalization */
    float max_db = -200.0f, min_db = 0.0f;
    for (int i = 0; i < bins; i++) {
        if (power_db[i] > max_db) max_db = power_db[i];
        if (power_db[i] < min_db) min_db = power_db[i];
    }
    min_db = max_db - 60.0f;

    /* Draw bars */
    int step = bins / WIDTH;
    for (int row = 9; row >= 0; row--) {
        fprintf(stderr, "  |");
        for (int col = 0; col < WIDTH; col++) {
            float avg = 0;
            for (int k = 0; k < step; k++) avg += power_db[col * step + k];
            avg /= step;
            float norm = (avg - min_db) / (max_db - min_db);
            int bar = (int)(norm * 10);
            fprintf(stderr, "%s", (bar >= row) ? "█" : " ");
        }
        fprintf(stderr, "|\n");
    }
    fprintf(stderr, "  +");
    for (int i = 0; i < WIDTH; i++) fprintf(stderr, "-");
    fprintf(stderr, "+\n");
    fprintf(stderr, "  Max: %.1f dB   Min: %.1f dB\n", max_db, min_db);
}

/* Simple FFT power spectrum (DFT for visualization only) */
static void compute_spectrum(int16_t *samples, int n, float *power_db, int bins) {
    int step = n / bins;
    for (int b = 0; b < bins; b++) {
        float re = 0, im = 0;
        int idx = b * step;
        float fi = (float)b / bins;
        for (int k = 0; k < step && idx + k * 2 + 1 < n; k++) {
            float angle = -2.0f * M_PI * fi * k;
            float i_s = samples[(idx + k) * 2]     / 2048.0f;
            float q_s = samples[(idx + k) * 2 + 1] / 2048.0f;
            re += i_s * cosf(angle) - q_s * sinf(angle);
            im += i_s * sinf(angle) + q_s * cosf(angle);
        }
        float power = re*re + im*im;
        power_db[b] = (power > 1e-10f) ? 10.0f * log10f(power) : -100.0f;
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sig);

    double freq_mhz = (argc > 1) ? atof(argv[1]) : 98.0;
    long long freq_hz = (long long)(freq_mhz * 1e6);

    fprintf(stderr, "FM Radio: %.1f MHz\n", freq_mhz);
    fprintf(stderr, "Connecting to PlutoSDR...\n");

    struct iio_context *ctx = iio_create_network_context("192.168.2.1");
    if (!ctx) { fprintf(stderr, "Cannot connect\n"); return 1; }

    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *rx  = iio_context_find_device(ctx, "cf-ad9361-lpc");
    if (!phy || !rx) { fprintf(stderr, "Devices not found\n"); return 1; }

    /* Configure RF */
    struct iio_channel *lo = iio_device_find_channel(phy, "altvoltage0", true);
    iio_channel_attr_write_longlong(lo, "frequency", freq_hz);

    struct iio_channel *rxch = iio_device_find_channel(phy, "voltage0", false);
    iio_channel_attr_write_longlong(rxch, "sampling_frequency", SAMPLE_RATE);
    iio_channel_attr_write_longlong(rxch, "rf_bandwidth", BANDWIDTH);
    iio_channel_attr_write(rxch, "gain_control_mode", "fast_attack");

    fprintf(stderr, "Tuned to %.3f MHz | %.1f MSPS | Auto gain\n",
            freq_mhz, SAMPLE_RATE / 1e6);
    fprintf(stderr, "Pipe audio to: aplay -r 48000 -f S16_LE -c 1\n");

    /* Enable IQ channels */
    struct iio_channel *rx_i = iio_device_find_channel(rx, "voltage0", false);
    struct iio_channel *rx_q = iio_device_find_channel(rx, "voltage1", false);
    iio_channel_enable(rx_i);
    iio_channel_enable(rx_q);

    struct iio_buffer *buf = iio_device_create_buffer(rx, BUF_SIZE, false);
    if (!buf) { fprintf(stderr, "Buffer failed\n"); return 1; }

    /* FM demod state */
    float prev_i = 0, prev_q = 0;
    float fir_buf[FIR_LEN] = {0};
    int fir_pos = 0;
    int decim_count = 0;
    int spectrum_count = 0;
    float power_db[60] = {0};

    fprintf(stderr, "Streaming... (Ctrl+C to stop)\n\n");

    while (!stop) {
        ssize_t nbytes = iio_buffer_refill(buf);
        if (nbytes < 0) { fprintf(stderr, "Refill error\n"); break; }

        int16_t *p   = (int16_t *)iio_buffer_start(buf);
        int16_t *end = (int16_t *)iio_buffer_end(buf);
        int n_samples = (end - p) / 2;

        /* Spectrum every 10 buffers */
        if (++spectrum_count >= 10) {
            compute_spectrum(p, n_samples < 512 ? n_samples : 512, power_db, 60);
            print_spectrum(power_db, 60, freq_mhz, SAMPLE_RATE / 1e6);
            spectrum_count = 0;
        }

        while (p < end) {
            float i_s = *p++ / 2048.0f;
            float q_s = *p++ / 2048.0f;

            /* FM differential demodulator, normalized by signal power */
            float demod = i_s * prev_q - q_s * prev_i;
            float power = i_s * i_s + q_s * q_s;
            if (power > 1e-6f) demod /= power;
            prev_i = i_s;
            prev_q = q_s;

            /* Low-pass FIR filter */
            fir_buf[fir_pos % FIR_LEN] = demod;
            fir_pos++;
            float filtered = 0;
            for (int k = 0; k < FIR_LEN; k++)
                filtered += fir[k] * fir_buf[(fir_pos + k) % FIR_LEN];

            /* Decimate to audio rate */
            if (++decim_count >= DECIM) {
                decim_count = 0;
                /* Scale: FM deviation ~75kHz / 2.5MSPS = 0.03 rad/sample */
                int16_t sample = (int16_t)(filtered * 32767.0f * 8.0f);
                fwrite(&sample, sizeof(int16_t), 1, stdout);
            }
        }
        fflush(stdout);
    }

    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    fprintf(stderr, "\nStopped.\n");
    return 0;
}
