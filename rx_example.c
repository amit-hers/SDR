#include <iio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>

#define SAMPLES     (1024 * 1024)
#define FREQ_HZ     100000000LL
#define SAMPLE_RATE 2500000
#define BANDWIDTH   2000000
#define GAIN_DB     50

static volatile bool stop = false;
static void handle_sig(int sig) { stop = true; }

int main() {
    signal(SIGINT, handle_sig);

    struct iio_context *ctx = iio_create_network_context("192.168.2.1");
    if (!ctx) { fprintf(stderr, "Cannot connect to PlutoSDR\n"); return 1; }

    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *rx  = iio_context_find_device(ctx, "cf-ad9361-lpc");

    if (!phy || !rx) {
        fprintf(stderr, "Devices not found\n");
        iio_context_destroy(ctx);
        return 1;
    }

    /* Configure LO frequency */
    struct iio_channel *lo = iio_device_find_channel(phy, "altvoltage0", true);
    iio_channel_attr_write_longlong(lo, "frequency", FREQ_HZ);

    /* Configure sample rate, bandwidth, gain */
    struct iio_channel *rxch = iio_device_find_channel(phy, "voltage0", false);
    iio_channel_attr_write_longlong(rxch, "sampling_frequency", SAMPLE_RATE);
    iio_channel_attr_write_longlong(rxch, "rf_bandwidth", BANDWIDTH);
    iio_channel_attr_write(rxch, "gain_control_mode", "manual");
    iio_channel_attr_write_longlong(rxch, "hardwaregain", GAIN_DB);

    printf("Tuned to %.3f MHz, %.1f MSPS\n", FREQ_HZ / 1e6, SAMPLE_RATE / 1e6);

    /* Enable I and Q channels on streaming device */
    struct iio_channel *rx_i = iio_device_find_channel(rx, "voltage0", false);
    struct iio_channel *rx_q = iio_device_find_channel(rx, "voltage1", false);
    iio_channel_enable(rx_i);
    iio_channel_enable(rx_q);

    /* Create buffer */
    struct iio_buffer *buf = iio_device_create_buffer(rx, SAMPLES, false);
    if (!buf) {
        fprintf(stderr, "Cannot create buffer\n");
        iio_context_destroy(ctx);
        return 1;
    }

    printf("Receiving %d samples...\n", SAMPLES);
    ssize_t nbytes = iio_buffer_refill(buf);
    if (nbytes < 0) {
        fprintf(stderr, "Buffer refill failed: %zd\n", nbytes);
        iio_buffer_destroy(buf);
        iio_context_destroy(ctx);
        return 1;
    }

    /* Process interleaved I/Q int16 samples */
    int16_t *start = (int16_t *)iio_buffer_start(buf);
    int16_t *end   = (int16_t *)iio_buffer_end(buf);
    int count = 0;
    double power_sum = 0.0;

    for (int16_t *p = start; p < end; p += 2) {
        double i = p[0] / 2048.0;
        double q = p[1] / 2048.0;
        power_sum += i*i + q*q;
        count++;
    }

    printf("Received %d IQ samples\n", count);
    printf("Average power: %.6f (linear)\n", power_sum / count);
    printf("Average power: %.2f dB\n", 10.0 * log10(power_sum / count));

    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    printf("Done.\n");
    return 0;
}
