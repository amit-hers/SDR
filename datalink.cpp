/*
 * datalink.cpp  —  SDR link daemon
 *
 * Modes:
 *   mesh      IP MANET mesh via TUN interface (FDD full-duplex)
 *   p2p-tx    Unidirectional COFDM transmitter
 *   p2p-rx    Unidirectional COFDM receiver
 *   l2bridge  Transparent Layer-2 bridge via TAP interface
 *   ranging   RSSI-based distance estimation
 *   scan      Interference avoidance / channel scan
 *   Adaptive mod    Auto BPSK/QPSK/16QAM/64QAM by SNR
 *   AES-256-CTR     Per-frame encryption (FIPS 140-2)
 *
 * Frame format (v2, sync=0xC0FFEE77, 22 bytes overhead):
 *   [SYNC  4B BE] [VER 1B=0x02] [FLAGS 1B] [MOD 1B] [BW 1B]
 *   [NODEID 4B BE] [SEQ 4B BE] [LEN 2B BE] [PAYLOAD N] [CRC32 4B LE]
 *
 * Adaptive modulation SNR thresholds:
 *   SNR < 6  dB → BPSK   (1 bps/sym)
 *   SNR < 13 dB → QPSK   (2 bps/sym)
 *   SNR < 22 dB → 16QAM  (4 bps/sym)
 *   SNR ≥ 22 dB → 64QAM  (6 bps/sym)
 *
 * Channel BW → sample rate:
 *   1.25 MHz → 2.5 MSPS  |  2.5 MHz → 5 MSPS  |  5 MHz → 10 MSPS
 *   10 MHz  → 20 MSPS    |  20 MHz  → 20 MSPS (PlutoSDR USB ceiling)
 *
 * Build:
 *   g++ -O2 -std=c++17 -pthread datalink.cpp -o datalink \
 *       -liio -lliquid -lssl -lcrypto -lm -lpthread
 *
 * Usage:
 *   sudo ./datalink --mode mesh    --freq-tx 434.0 --freq-rx 439.0 --bw 10
 *        ./datalink --mode p2p-tx  --freq 434.0 --bw 20 --mod QPSK
 *        ./datalink --mode p2p-rx  --freq 434.0 --bw 20 --mod AUTO
 *        ./datalink --mode scan    --freq 430.0 --scan-step 1.0 --scan-n 20
 *        ./datalink --mode ranging --freq 434.0 --tx-power 0 --tx-gain 10
 */

#include <iio.h>
#include <liquid/liquid.h>
#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifdef __linux__
#include <linux/if_tun.h>
#endif

#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <array>
#include <string>
#include <chrono>
#include <algorithm>

/* ── Frame constants ────────────────────────────────────────── */
#define DATALINK_SYNC    0xC0FFEE77U   /* distinguishes from P2P 0xDEADBEEF */
#define DATALINK_VER     0x02
#define MAX_PAYLOAD  1400          /* IPv4 MTU */
#define SPS          4
#define ROLLOFF      0.35f
#define RRC_TAPS     16
#define IIO_BUFSZ    (64*1024)

/* Flags byte */
#define FL_ENCRYPT   0x01
#define FL_AUDIO     0x02
#define FL_CONTROL   0x04   /* control / ranging frame */
#define FL_ACK       0x08   /* ranging echo */
#define FL_ROUTE     0x10   /* mesh route advertisement */

/* Modulation codes (stored in frame header) */
#define MOD_BPSK   1
#define MOD_QPSK   2
#define MOD_16QAM  3
#define MOD_64QAM  4

/* Bandwidth codes */
#define BW_20MHZ   0
#define BW_10MHZ   1
#define BW_5MHZ    2
#define BW_2P5MHZ  3
#define BW_1P25MHZ 4

/* ── Mode enum ──────────────────────────────────────────────── */
enum class Mode { P2P_TX, P2P_RX, MESH, L2BRIDGE, P2MP, SCAN, RANGING };

/* ── Config ─────────────────────────────────────────────────── */
struct Config {
    Mode        mode        = Mode::P2P_TX;
    double      freq_tx     = 434.0;    /* MHz */
    double      freq_rx     = 434.0;    /* MHz (for FDD mesh) */
    int         bw_code     = BW_20MHZ;
    int         mod_code    = MOD_QPSK; /* -1 = AUTO */
    bool        adaptive    = false;
    bool        encrypt     = false;
    uint8_t     aes_key[32] = {};
    int         tx_atten    = 10;       /* dB */
    float       tx_power_dbm= -10.0f;  /* derived from atten */
    float       g_tx        = 10.0f;   /* TX antenna gain dBi */
    float       g_rx        = 10.0f;   /* RX antenna gain dBi */
    const char* pluto_ip    = "192.168.2.1";
    /* Channel scan */
    double      scan_start  = 430.0;   /* MHz */
    double      scan_step   = 1.0;     /* MHz */
    int         scan_n      = 10;
    /* TUN/TAP */
    const char* iface       = "datalink";
};

/* ── Globals ─────────────────────────────────────────────────── */
static std::atomic<bool>  g_stop{false};
static std::atomic<int>   g_new_atten{-1};
static std::atomic<float> g_new_freq{-1.0f};
static std::atomic<int>   g_rx_mod{MOD_QPSK};     /* RX reports current demod */
static std::atomic<float> g_rx_snr{0.0f};          /* RX reports SNR */
static iio_channel*       g_txch  = nullptr;
static iio_channel*       g_lo_tx = nullptr;

static void on_sig(int)  { g_stop.store(true); }
static void on_usr1(int) {
    FILE* f = fopen("/tmp/datalink_ctrl", "r");
    if (!f) return;
    int a=-1; float fr=-1.0f;
    int _r = fscanf(f, "%d %f", &a, &fr); (void)_r;
    fclose(f);
    if (a >= 0 && a <= 89) g_new_atten.store(a);
    if (fr > 0.0f)         g_new_freq.store(fr);
}

/* ═══════════════════════════════════════════════════════════════
 * CRC-32
 * ═══════════════════════════════════════════════════════════════ */
static uint32_t crc_tab[256];
static void crc_init() {
    for (int i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c&1) ? (0xEDB88320U^(c>>1)) : (c>>1);
        crc_tab[i] = c;
    }
}
static uint32_t crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c^d[i])&0xFF]^(c>>8);
    return c ^ 0xFFFFFFFFU;
}

/* ═══════════════════════════════════════════════════════════════
 * AES-256-CTR
 * ═══════════════════════════════════════════════════════════════ */
static void aes_ctr(const uint8_t* key, uint64_t ctr,
                    const uint8_t* in, uint8_t* out, int len) {
    uint8_t iv[16] = {};
    memcpy(iv+8, &ctr, 8);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int outl = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv);
    EVP_EncryptUpdate(ctx, out, &outl, in, len);
    EVP_CIPHER_CTX_free(ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * Frame encode / decode
 * ═══════════════════════════════════════════════════════════════ */
static uint32_t g_node_id = 0;   /* set in main() from hostname hash */
static uint32_t g_tx_seq  = 0;

/* build_datalink_frame: returns total frame bytes.
 * out must be at least plen + 22 bytes. */
static int build_datalink_frame(const uint8_t* pl, int plen,
                             uint8_t flags, uint8_t mod, uint8_t bw,
                             bool enc, const uint8_t* key, uint64_t ctr,
                             uint8_t* out) {
    uint8_t enc_buf[MAX_PAYLOAD];
    if (enc) { aes_ctr(key, ctr, pl, enc_buf, plen); pl = enc_buf; }

    int p = 0;
    /* SYNC big-endian */
    out[p++]=(DATALINK_SYNC>>24)&0xFF; out[p++]=(DATALINK_SYNC>>16)&0xFF;
    out[p++]=(DATALINK_SYNC>> 8)&0xFF; out[p++]= DATALINK_SYNC     &0xFF;
    out[p++] = DATALINK_VER;
    out[p++] = flags | (enc ? FL_ENCRYPT : 0);
    out[p++] = mod;
    out[p++] = bw;
    /* node ID big-endian */
    uint32_t nid = g_node_id;
    out[p++]=(nid>>24)&0xFF; out[p++]=(nid>>16)&0xFF;
    out[p++]=(nid>> 8)&0xFF; out[p++]= nid     &0xFF;
    /* sequence big-endian */
    uint32_t seq = g_tx_seq++;
    out[p++]=(seq>>24)&0xFF; out[p++]=(seq>>16)&0xFF;
    out[p++]=(seq>> 8)&0xFF; out[p++]= seq     &0xFF;
    /* length big-endian */
    out[p++]=(uint8_t)(plen>>8); out[p++]=(uint8_t)plen;
    /* payload */
    memcpy(out+p, pl, plen); p += plen;
    /* CRC over everything so far */
    uint32_t c = crc32(out, p);
    memcpy(out+p, &c, 4); p += 4;
    return p;
}

/* DATALINK frame decoder state machine */
struct DatalinkDecoder {
    enum class S { HUNT, HDR, PAYLOAD } state = S::HUNT;
    uint32_t shift   = 0;
    uint8_t  buf[MAX_PAYLOAD + 32] = {};
    int      pos     = 0;
    int      expect  = 0;
    uint16_t plen    = 0;
    uint8_t  flags   = 0;
    uint8_t  mod     = 0;
    uint8_t  bw      = 0;
    uint32_t node_id = 0;
    uint32_t seq     = 0;
    uint64_t frames_good = 0, frames_bad = 0;

    /* Returns payload length on complete valid frame, -1 on CRC fail, 0 otherwise */
    int push(uint8_t b, uint8_t* payload_out,
             bool enc, const uint8_t* key, uint64_t& frame_ctr) {
        switch (state) {
        case S::HUNT:
            shift = (shift << 8) | b;
            if (shift == DATALINK_SYNC) {
                buf[0]=(DATALINK_SYNC>>24)&0xFF; buf[1]=(DATALINK_SYNC>>16)&0xFF;
                buf[2]=(DATALINK_SYNC>> 8)&0xFF; buf[3]= DATALINK_SYNC     &0xFF;
                pos=4; state=S::HDR; expect=14; /* VER..LEN = 14 bytes */
            }
            break;
        case S::HDR:
            buf[pos++] = b;
            if (--expect == 0) {
                /* pos is now 18 */
                flags   = buf[5];
                mod     = buf[6];
                bw      = buf[7];
                node_id = ((uint32_t)buf[ 8]<<24)|((uint32_t)buf[ 9]<<16)
                         |((uint32_t)buf[10]<< 8)| buf[11];
                seq     = ((uint32_t)buf[12]<<24)|((uint32_t)buf[13]<<16)
                         |((uint32_t)buf[14]<< 8)| buf[15];
                plen    = ((uint16_t)buf[16]<<8)| buf[17];
                if (plen > MAX_PAYLOAD) { state=S::HUNT; shift=0; break; }
                expect  = plen + 4; state = S::PAYLOAD;
            }
            break;
        case S::PAYLOAD:
            buf[pos++] = b;
            if (--expect == 0) {
                int hlen = 18 + plen;
                uint32_t got, exp;
                memcpy(&got, buf+hlen, 4);
                exp = crc32(buf, hlen);
                state = S::HUNT; shift = 0; pos = 0;
                if (got != exp) { frames_bad++; return -1; }
                frames_good++;
                const uint8_t* pl = buf + 18;
                if ((flags & FL_ENCRYPT) && enc) {
                    aes_ctr(key, frame_ctr, pl, payload_out, plen);
                    frame_ctr++;
                } else {
                    memcpy(payload_out, pl, plen);
                }
                return plen;
            }
            break;
        }
        return 0;
    }
};

/* ═══════════════════════════════════════════════════════════════
 * Adaptive modulation
 * ═══════════════════════════════════════════════════════════════ */
struct AdaptMod {
    static modulation_scheme code_to_scheme(int code) {
        switch(code) {
        case MOD_BPSK:  return LIQUID_MODEM_BPSK;
        case MOD_16QAM: return LIQUID_MODEM_QAM16;
        case MOD_64QAM: return LIQUID_MODEM_QAM64;
        default:        return LIQUID_MODEM_QPSK;
        }
    }
    static int scheme_to_code(modulation_scheme s) {
        switch(s) {
        case LIQUID_MODEM_BPSK:  return MOD_BPSK;
        case LIQUID_MODEM_QAM16: return MOD_16QAM;
        case LIQUID_MODEM_QAM64: return MOD_64QAM;
        default:                 return MOD_QPSK;
        }
    }
    static int bps(int code) {
        switch(code) {
        case MOD_BPSK:  return 1;
        case MOD_16QAM: return 4;
        case MOD_64QAM: return 6;
        default:        return 2; /* QPSK */
        }
    }
    /* Returns new mod code given current SNR (with 2dB hysteresis) */
    static int snr_to_code(float snr, int current) {
        /* Switch-up thresholds (plus 2 dB margin) */
        if (snr >= 24.0f && current < MOD_64QAM) return MOD_64QAM;
        if (snr >= 15.0f && current < MOD_16QAM) return MOD_16QAM;
        if (snr >= 8.0f  && current < MOD_QPSK)  return MOD_QPSK;
        /* Switch-down thresholds (minus 1 dB margin) */
        if (snr <  5.0f  && current > MOD_BPSK)  return MOD_BPSK;
        if (snr <  12.0f && current > MOD_QPSK)  return MOD_QPSK;
        if (snr <  21.0f && current > MOD_16QAM) return MOD_16QAM;
        return current;
    }
    static const char* name(int code) {
        switch(code) {
        case MOD_BPSK:  return "BPSK";
        case MOD_16QAM: return "16QAM";
        case MOD_64QAM: return "64QAM";
        default:        return "QPSK";
        }
    }
};

/* ═══════════════════════════════════════════════════════════════
 * BW code → sample rate (Hz)
 * ═══════════════════════════════════════════════════════════════ */
static long long bw_to_rate(int bw_code) {
    switch(bw_code) {
    case BW_1P25MHZ: return  2500000LL;
    case BW_2P5MHZ:  return  5000000LL;
    case BW_5MHZ:    return 10000000LL;
    case BW_10MHZ:   return 20000000LL;
    default:         return 20000000LL; /* 20 MHz */
    }
}
static float bw_mhz(int bw_code) {
    switch(bw_code) {
    case BW_1P25MHZ: return  1.25f;
    case BW_2P5MHZ:  return  2.5f;
    case BW_5MHZ:    return  5.0f;
    case BW_10MHZ:   return 10.0f;
    default:         return 20.0f;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * TUN / TAP interface (Linux)
 * ═══════════════════════════════════════════════════════════════ */
#ifdef __linux__
static int tuntap_open(const char* ifname, bool tap) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { perror("[datalink] /dev/net/tun"); return -1; }
    struct ifreq ifr = {};
    ifr.ifr_flags = (tap ? IFF_TAP : IFF_TUN) | IFF_NO_PI;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("[datalink] TUNSETIFF"); close(fd); return -1;
    }
    /* Bring interface up */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr2 = {};
    strncpy(ifr2.ifr_name, ifname, IFNAMSIZ-1);
    ioctl(s, SIOCGIFFLAGS, &ifr2);
    ifr2.ifr_flags |= IFF_UP | IFF_RUNNING;
    ioctl(s, SIOCSIFFLAGS, &ifr2);
    close(s);
    fprintf(stderr, "[datalink] %s interface '%s' up — set IP with:\n"
                    "       sudo ip addr add 10.0.8.1/24 dev %s\n",
            tap ? "TAP" : "TUN", ifname, ifname);
    return fd;
}
#else
static int tuntap_open(const char*, bool) {
    fprintf(stderr, "[datalink] TUN/TAP only supported on Linux\n");
    return -1;
}
#endif

/* ═══════════════════════════════════════════════════════════════
 * SPSC ring buffers
 * ═══════════════════════════════════════════════════════════════ */
template<size_t SLOTS>
class FrameRing {
    struct alignas(64) Slot { uint8_t data[MAX_PAYLOAD+32]; int len=0; };
    Slot slots_[SLOTS];
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    static constexpr size_t MASK = SLOTS-1;
    static_assert((SLOTS&(SLOTS-1))==0,"power of 2");
public:
    bool push(const uint8_t* d, int l) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == SLOTS) return false;
        auto& s = slots_[h & MASK]; memcpy(s.data,d,l); s.len=l;
        head_.store(h+1, std::memory_order_release); return true;
    }
    struct Slot* peek() {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return nullptr;
        return &slots_[t & MASK];
    }
    void consume() { tail_.fetch_add(1, std::memory_order_release); }
    size_t size() const {
        return (size_t)(head_.load()-tail_.load(std::memory_order_relaxed));
    }
};

template<size_t SLOTS>
class IQRing {
    struct alignas(64) Slot { std::vector<int16_t> data; };
    Slot slots_[SLOTS];
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    static constexpr size_t MASK = SLOTS-1;
    static_assert((SLOTS&(SLOTS-1))==0,"power of 2");
public:
    bool push(const int16_t* p, size_t n) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == SLOTS) return false;
        slots_[h&MASK].data.assign(p,p+n);
        head_.store(h+1, std::memory_order_release); return true;
    }
    struct Slot* peek() {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return nullptr;
        return &slots_[t & MASK];
    }
    void consume() { tail_.fetch_add(1, std::memory_order_release); }
};

/* ═══════════════════════════════════════════════════════════════
 * Stats
 * ═══════════════════════════════════════════════════════════════ */
struct DatalinkStats {
    std::atomic<uint64_t> frames_tx{0};
    std::atomic<uint64_t> frames_rx_good{0};
    std::atomic<uint64_t> frames_rx_bad{0};
    std::atomic<uint64_t> bytes_tx{0};
    std::atomic<uint64_t> bytes_rx{0};
    std::atomic<uint64_t> dropped{0};
    float rssi_db   = 0.0f;
    float snr_db    = 0.0f;
    int   cur_mod   = MOD_QPSK;
};

static void write_stats(const Config& c, const DatalinkStats& s,
                        double uptime, float tx_kbps, float rx_kbps) {
    /* Ranging: RSSI-based distance model
     * FSPL(d,f) = 20log10(d_km) + 20log10(f_MHz) + 20log10(4π/c * 1e9) */
    float fspl_ref = 20.0f*log10f((float)c.freq_rx) + 147.55f;
    float rssi = s.rssi_db;
    float dist_km = 0.0f;
    if (rssi < -10.0f) {
        float path_db = c.tx_power_dbm + c.g_tx + c.g_rx - rssi;
        dist_km = powf(10.0f, (path_db - fspl_ref) / 20.0f);
    }

    FILE* f = fopen("/tmp/datalink_stats.json", "w");
    if (!f) return;
    fprintf(f,
        "{\"mode\":\"%s\","
        "\"freq_tx\":%.3f,\"freq_rx\":%.3f,\"bw_mhz\":%.2f,"
        "\"mod\":\"%s\",\"adaptive\":%s,"
        "\"frames_tx\":%llu,\"frames_rx_good\":%llu,\"frames_rx_bad\":%llu,"
        "\"dropped\":%llu,"
        "\"tx_kbps\":%.1f,\"rx_kbps\":%.1f,"
        "\"rssi_db\":%.1f,\"snr_db\":%.1f,"
        "\"tx_power_dbm\":%.1f,\"tx_power_mw\":%.4f,"
        "\"rx_power_mw\":%.6f,"
        "\"dist_km\":%.3f,"
        "\"node_id\":\"%08X\","
        "\"uptime_s\":%.0f,\"status\":\"running\"}\n",
        (c.mode==Mode::MESH?"mesh":c.mode==Mode::L2BRIDGE?"l2bridge":
         c.mode==Mode::P2P_TX?"p2p-tx":c.mode==Mode::P2P_RX?"p2p-rx":"p2mp"),
        c.freq_tx, c.freq_rx, bw_mhz(c.bw_code),
        AdaptMod::name(s.cur_mod), c.adaptive?"true":"false",
        (unsigned long long)s.frames_tx.load(),
        (unsigned long long)s.frames_rx_good.load(),
        (unsigned long long)s.frames_rx_bad.load(),
        (unsigned long long)s.dropped.load(),
        tx_kbps, rx_kbps,
        rssi, s.snr_db,
        c.tx_power_dbm, powf(10.0f, c.tx_power_dbm/10.0f),
        powf(10.0f, rssi/10.0f),
        dist_km,
        g_node_id,
        uptime);
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════
 * IIO context helpers
 * ═══════════════════════════════════════════════════════════════ */
struct IIOCtx {
    iio_context* ctx   = nullptr;
    iio_device*  phy   = nullptr;
    iio_device*  tx_dev= nullptr;
    iio_device*  rx_dev= nullptr;
    iio_channel* lo_tx = nullptr;
    iio_channel* lo_rx = nullptr;
    iio_channel* txch  = nullptr;
    iio_channel* rxch  = nullptr;
    iio_buffer*  tx_buf= nullptr;
    iio_buffer*  rx_buf= nullptr;
};

static bool iio_setup(IIOCtx& io, const Config& c, bool need_tx, bool need_rx) {
    io.ctx = iio_create_network_context(c.pluto_ip);
    if (!io.ctx) { fprintf(stderr,"[datalink] Cannot connect to %s\n",c.pluto_ip); return false; }

    io.phy    = iio_context_find_device(io.ctx, "ad9361-phy");
    io.tx_dev = iio_context_find_device(io.ctx, "cf-ad9361-dds-core-lpc");
    io.rx_dev = iio_context_find_device(io.ctx, "cf-ad9361-lpc");
    if (!io.phy||!io.tx_dev||!io.rx_dev) {
        fprintf(stderr,"[datalink] Devices not found\n"); return false;
    }

    long long rate = bw_to_rate(c.bw_code);
    long long bw   = (long long)(bw_mhz(c.bw_code) * 0.9e6); /* 90% of BW */

    if (need_tx) {
        io.lo_tx = iio_device_find_channel(io.phy, "altvoltage1", true);
        iio_channel_attr_write_longlong(io.lo_tx, "frequency",
                                       (long long)(c.freq_tx * 1e6));
        io.txch  = iio_device_find_channel(io.phy, "voltage0", true);
        iio_channel_attr_write_longlong(io.txch, "sampling_frequency", rate);
        iio_channel_attr_write_longlong(io.txch, "rf_bandwidth",       bw);
        iio_channel_attr_write_longlong(io.txch, "hardwaregain",       -c.tx_atten);
        iio_channel_enable(iio_device_find_channel(io.tx_dev,"voltage0",true));
        iio_channel_enable(iio_device_find_channel(io.tx_dev,"voltage1",true));
        io.tx_buf = iio_device_create_buffer(io.tx_dev, IIO_BUFSZ, false);
        g_txch  = io.txch;
        g_lo_tx = io.lo_tx;
    }
    if (need_rx) {
        io.lo_rx = iio_device_find_channel(io.phy, "altvoltage0", true);
        iio_channel_attr_write_longlong(io.lo_rx, "frequency",
                                       (long long)(c.freq_rx * 1e6));
        io.rxch  = iio_device_find_channel(io.phy, "voltage0", false);
        iio_channel_attr_write_longlong(io.rxch, "sampling_frequency", rate);
        iio_channel_attr_write_longlong(io.rxch, "rf_bandwidth",       bw);
        iio_channel_attr_write(io.rxch, "gain_control_mode", "fast_attack");
        iio_channel_enable(iio_device_find_channel(io.rx_dev,"voltage0",false));
        iio_channel_enable(iio_device_find_channel(io.rx_dev,"voltage1",false));
        io.rx_buf = iio_device_create_buffer(io.rx_dev, IIO_BUFSZ, false);
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * TX path: frame bytes → IQ → IIO
 * ═══════════════════════════════════════════════════════════════ */
static void tx_frames(IIOCtx& io, FrameRing<64>& ring,
                      const Config& c, DatalinkStats& stats,
                      std::atomic<int>& cur_mod_code) {
    int code    = cur_mod_code.load();
    modulation_scheme scheme = AdaptMod::code_to_scheme(code);
    modemcf        mod    = modemcf_create(scheme);
    firinterp_crcf interp = firinterp_crcf_create_prototype(
                               LIQUID_FIRFILT_RRC, SPS, RRC_TAPS, ROLLOFF, 0);
    std::vector<int16_t> iq;
    iq.reserve(IIO_BUFSZ * 2);
    liquid_float_complex iq_out[SPS];
    uint64_t frame_ctr = 0;
    uint8_t  frame_buf[MAX_PAYLOAD + 32];

    while (!g_stop.load(std::memory_order_relaxed)) {
        /* Check for modulation change */
        int new_code = cur_mod_code.load(std::memory_order_relaxed);
        if (new_code != code) {
            code   = new_code; scheme = AdaptMod::code_to_scheme(code);
            modemcf_destroy(mod); mod = modemcf_create(scheme);
            fprintf(stderr,"\n[datalink-tx] Modulation → %s\n", AdaptMod::name(code));
        }
        /* Check for hardware updates */
        int na = g_new_atten.exchange(-1);
        if (na>=0 && io.txch) {
            iio_channel_attr_write_longlong(io.txch,"hardwaregain",-na);
            fprintf(stderr,"\n[datalink-tx] Atten=%d dB\n",na);
        }
        float nf = g_new_freq.exchange(-1.0f);
        if (nf>0.0f && io.lo_tx) {
            iio_channel_attr_write_longlong(io.lo_tx,"frequency",(long long)(nf*1e6));
            fprintf(stderr,"\n[datalink-tx] Freq=%.3f MHz\n",nf);
        }

        auto* slot = ring.peek();
        if (!slot) { std::this_thread::yield(); continue; }

        int bps  = AdaptMod::bps(code);
        int flen = build_datalink_frame(slot->data, slot->len,
                                    0, (uint8_t)code, (uint8_t)c.bw_code,
                                    c.encrypt, c.aes_key, frame_ctr++,
                                    frame_buf);
        ring.consume();
        stats.frames_tx.fetch_add(1);
        stats.bytes_tx.fetch_add(slot->len);

        int n_bits = flen * 8;
        int n_sym  = (n_bits + bps - 1) / bps;
        iq.clear();

        for (int sym = 0; sym < n_sym; sym++) {
            unsigned s = 0;
            for (int b = 0; b < bps; b++) {
                int bit = sym*bps+b, bi = bit/8, bo = 7-(bit%8);
                s = (s<<1) | ((bi < flen) ? ((frame_buf[bi]>>bo)&1) : 0);
            }
            liquid_float_complex iq_sym;
            modemcf_modulate(mod, s, &iq_sym);
            firinterp_crcf_execute(interp, iq_sym, iq_out);
            for (int k = 0; k < SPS; k++) {
                iq.push_back((int16_t)(iq_out[k].real * 2047.0f));
                iq.push_back((int16_t)(iq_out[k].imag * 2047.0f));
            }
        }

        int16_t* dst = (int16_t*)iio_buffer_start(io.tx_buf);
        int      cap = (int)((int16_t*)iio_buffer_end(io.tx_buf) - dst);
        int      cp  = (int)iq.size() < cap ? (int)iq.size() : cap;
        memcpy(dst, iq.data(), cp * sizeof(int16_t));
        if (cp < cap) memset(dst+cp, 0, (cap-cp)*sizeof(int16_t));
        if (iio_buffer_push(io.tx_buf) < 0)
            stats.dropped.fetch_add(1);
    }
    firinterp_crcf_destroy(interp);
    modemcf_destroy(mod);
}

/* ═══════════════════════════════════════════════════════════════
 * RX path: IIO → IQ → frames
 * Returns decoded payload via callback
 * ═══════════════════════════════════════════════════════════════ */
static void rx_frames(IIOCtx& io, const Config& c, DatalinkStats& stats,
                      std::atomic<int>& cur_mod_code,
                      std::function<void(const uint8_t*,int,uint8_t)> on_frame) {
    int code = cur_mod_code.load();
    modulation_scheme scheme = AdaptMod::code_to_scheme(code);

    agc_crcf      agc   = agc_crcf_create();
    agc_crcf_set_bandwidth(agc, 1e-3f);
    firdecim_crcf decim = firdecim_crcf_create_prototype(
                           LIQUID_FIRFILT_RRC, SPS, RRC_TAPS, ROLLOFF, 0);
    symsync_crcf  sync  = symsync_crcf_create_rnyquist(
                           LIQUID_FIRFILT_RRC, SPS, RRC_TAPS, ROLLOFF, 32);
    symsync_crcf_set_lf_bw(sync, 0.02f);
    nco_crcf      nco   = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(nco, 0.04f);
    modemcf       demod = modemcf_create(scheme);
    DatalinkDecoder   dec;

    liquid_float_complex decim_in[SPS]; int didx=0;
    uint8_t  cur_byte=0; int bit_cursor=0;
    uint8_t  out_buf[MAX_PAYLOAD+32];
    uint64_t frame_ctr=0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        /* Switch modulation if changed */
        int new_code = cur_mod_code.load(std::memory_order_relaxed);
        if (new_code != code) {
            code   = new_code; scheme = AdaptMod::code_to_scheme(code);
            modemcf_destroy(demod); demod = modemcf_create(scheme);
        }
        int bps = AdaptMod::bps(code);

        ssize_t nr = iio_buffer_refill(io.rx_buf);
        if (nr <= 0) continue;

        const int16_t* p   = (const int16_t*)iio_buffer_start(io.rx_buf);
        const int16_t* end = (const int16_t*)iio_buffer_end(io.rx_buf);

        while (p < end) {
            liquid_float_complex samp{*p/2047.0f, *(p+1)/2047.0f};
            p += 2;
            liquid_float_complex agc_out;
            agc_crcf_execute(agc, samp, &agc_out);
            stats.rssi_db = 0.999f*stats.rssi_db + 0.001f*agc_crcf_get_rssi(agc);
            decim_in[didx++] = agc_out;
            if (didx < SPS) continue; didx=0;

            liquid_float_complex decim_out;
            firdecim_crcf_execute(decim, decim_in, &decim_out);
            decim_out.real /= SPS; decim_out.imag /= SPS;

            liquid_float_complex sync_out[8]; unsigned ns=0;
            symsync_crcf_execute(sync, &decim_out, 1, sync_out, &ns);

            for (unsigned si=0; si<ns; si++) {
                liquid_float_complex ph;
                nco_crcf_mix_down(nco, sync_out[si], &ph);
                nco_crcf_step(nco);
                float re=ph.real, im=ph.imag;
                float sr=(re>=0)?1.f:-1.f, sq=(im>=0)?1.f:-1.f;
                nco_crcf_pll_step(nco, sr*im - sq*re);

                unsigned sym;
                modemcf_demodulate(demod, ph, &sym);
                float evm = modemcf_get_demodulator_evm(demod);
                if (evm > 1e-6f)
                    stats.snr_db = 0.999f*stats.snr_db
                                 + 0.001f*10.0f*log10f(1.0f/(evm*evm+1e-12f));

                for (int b = bps-1; b >= 0; b--) {
                    cur_byte = (uint8_t)((cur_byte<<1)|((sym>>b)&1));
                    if (++bit_cursor == 8) {
                        int plen = dec.push(cur_byte, out_buf,
                                            c.encrypt, c.aes_key, frame_ctr);
                        if (plen > 0) {
                            stats.frames_rx_good.fetch_add(1);
                            stats.bytes_rx.fetch_add(plen);
                            on_frame(out_buf, plen, dec.flags);
                        } else if (plen < 0) {
                            stats.frames_rx_bad.fetch_add(1);
                        }
                        cur_byte=0; bit_cursor=0;
                    }
                }
            }
        }
    }
    modemcf_destroy(demod); nco_crcf_destroy(nco);
    symsync_crcf_destroy(sync); firdecim_crcf_destroy(decim);
    agc_crcf_destroy(agc);
}

/* ═══════════════════════════════════════════════════════════════
 * RSSI-based ranging (no RTT, uses link budget model)
 * dist_km = 10^( (P_tx + G_tx + G_rx - RSSI - FSPL_ref) / 20 )
 * FSPL_ref = 20log10(f_MHz) + 147.55 dB  (for d in km)
 * ═══════════════════════════════════════════════════════════════ */
float rssi_to_dist_km(float rssi_dbm, float freq_mhz,
                      float tx_dbm, float g_tx, float g_rx) {
    float fspl_ref = 20.0f*log10f(freq_mhz) + 147.55f;
    float path_db  = tx_dbm + g_tx + g_rx - rssi_dbm;
    return powf(10.0f, (path_db - fspl_ref) / 20.0f);
}

/* ═══════════════════════════════════════════════════════════════
 * Channel scanner
 * ═══════════════════════════════════════════════════════════════ */
static void run_scan(const Config& c) {
    iio_context* ctx = iio_create_network_context(c.pluto_ip);
    if (!ctx) return;
    iio_device* phy    = iio_context_find_device(ctx, "ad9361-phy");
    iio_device* rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    iio_channel* lo_rx = iio_device_find_channel(phy, "altvoltage0", true);
    iio_channel* rxch  = iio_device_find_channel(phy, "voltage0", false);
    iio_channel_attr_write_longlong(rxch, "sampling_frequency", 2500000LL);
    iio_channel_attr_write_longlong(rxch, "rf_bandwidth",       1200000LL);
    iio_channel_attr_write(rxch, "gain_control_mode", "fast_attack");
    iio_channel_enable(iio_device_find_channel(rx_dev,"voltage0",false));
    iio_channel_enable(iio_device_find_channel(rx_dev,"voltage1",false));
    iio_buffer* buf = iio_device_create_buffer(rx_dev, 4096, false);

    FILE* out = fopen("/tmp/datalink_scan.json","w");
    if (out) fprintf(out, "{\"channels\":[");

    printf("%-12s  %s\n", "Freq (MHz)", "Power (dBm)");
    printf("%-12s  %s\n", "----------", "-----------");

    for (int i = 0; i < c.scan_n && !g_stop.load(); i++) {
        double freq = c.scan_start + i * c.scan_step;
        iio_channel_attr_write_longlong(lo_rx, "frequency", (long long)(freq*1e6));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        float pwr = 0.0f; int nsamp = 0;
        for (int t = 0; t < 3; t++) {
            iio_buffer_refill(buf);
            const int16_t* p   = (const int16_t*)iio_buffer_start(buf);
            const int16_t* end = (const int16_t*)iio_buffer_end(buf);
            while (p < end) {
                float I = *p/2047.0f, Q = *(p+1)/2047.0f;
                pwr += I*I + Q*Q; nsamp++; p+=2;
            }
        }
        float pwr_dbm = 10.0f*log10f(pwr/nsamp + 1e-10f);
        const char* bar_ch = (pwr_dbm > -60) ? "█" : (pwr_dbm > -80) ? "▓" : "░";
        int bar = (int)((pwr_dbm + 100) / 5); if (bar<0) bar=0; if (bar>20) bar=20;
        printf("%-12.3f  %.1f dBm  ", freq, pwr_dbm);
        for (int b=0;b<bar;b++) printf("%s",bar_ch);
        printf("\n");
        if (out) fprintf(out, "%s{\"freq\":%.3f,\"power_dbm\":%.1f}",
                         i?",":"", freq, pwr_dbm);
    }
    if (out) { fprintf(out,"]}"); fclose(out); }

    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * P2P TX mode
 * ═══════════════════════════════════════════════════════════════ */
static void run_p2p_tx(const Config& c) {
    IIOCtx io;
    if (!iio_setup(io, c, true, false)) return;

    FrameRing<64> ring;
    DatalinkStats     stats;
    stats.cur_mod = c.mod_code;
    std::atomic<int> cur_mod{c.mod_code};
    auto t0 = std::chrono::steady_clock::now();
    auto t_stats = t0;
    uint8_t buf[MAX_PAYLOAD];

    /* TX thread */
    std::thread tx_thr(tx_frames, std::ref(io), std::ref(ring),
                       std::cref(c), std::ref(stats), std::ref(cur_mod));

    /* Main: read stdin → frame queue */
    while (!g_stop.load()) {
        int len = (int)fread(buf, 1, MAX_PAYLOAD, stdin);
        if (len == 0) { g_stop.store(true); break; }
        while (!ring.push(buf, len) && !g_stop.load())
            std::this_thread::yield();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t_stats).count();
        if (dt >= 2.0) {
            double up = std::chrono::duration<double>(now - t0).count();
            float kbps = (float)(stats.bytes_tx.exchange(0)*8.0/dt/1000.0);
            if (c.adaptive) {
                int nm = AdaptMod::snr_to_code(g_rx_snr.load(), cur_mod.load());
                if (nm != cur_mod.load()) cur_mod.store(nm);
            }
            stats.cur_mod = cur_mod.load();
            write_stats(c, stats, up, kbps, 0.0f);
            fprintf(stderr,"\r[datalink-tx] sent=%-7llu drop=%-4llu %.1f kbps  %s   ",
                    (unsigned long long)stats.frames_tx.load(),
                    (unsigned long long)stats.dropped.load(),
                    kbps, AdaptMod::name(cur_mod.load()));
            t_stats = now;
        }
    }
    g_stop.store(true);
    tx_thr.join();
    if (io.tx_buf) iio_buffer_destroy(io.tx_buf);
    iio_context_destroy(io.ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * P2P RX mode
 * ═══════════════════════════════════════════════════════════════ */
static void run_p2p_rx(const Config& c) {
    IIOCtx io;
    if (!iio_setup(io, c, false, true)) return;

    DatalinkStats     stats;
    stats.cur_mod = c.mod_code;
    std::atomic<int> cur_mod{c.mod_code};
    auto t0 = std::chrono::steady_clock::now();
    auto t_stats = t0;

    auto on_frame = [&](const uint8_t* data, int len, uint8_t flags) {
        if (!(flags & FL_CONTROL) && !(flags & FL_ACK)) {
            fwrite(data, 1, len, stdout);
            fflush(stdout);
        }
    };

    /* Stats writer thread */
    std::thread stats_thr([&](){
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto now = std::chrono::steady_clock::now();
            double up  = std::chrono::duration<double>(now - t0).count();
            float kbps = (float)(stats.bytes_rx.exchange(0)*8.0/2.0/1000.0);
            g_rx_snr.store(stats.snr_db);
            stats.cur_mod = cur_mod.load();
            write_stats(c, stats, up, 0.0f, kbps);
            fprintf(stderr,
                "\r[datalink-rx] good=%-7llu bad=%-5llu RSSI=%+.1fdB SNR=%.1fdB %.1f kbps  %s   ",
                (unsigned long long)stats.frames_rx_good.load(),
                (unsigned long long)stats.frames_rx_bad.load(),
                stats.rssi_db, stats.snr_db, kbps,
                AdaptMod::name(cur_mod.load()));
        }
    });

    rx_frames(io, c, stats, cur_mod, on_frame);
    g_stop.store(true);
    stats_thr.join();
    if (io.rx_buf) iio_buffer_destroy(io.rx_buf);
    iio_context_destroy(io.ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * MESH / L2BRIDGE mode  (full-duplex FDD: TX on freq_tx, RX on freq_rx)
 * Data source/sink: TUN (IP) or TAP (Ethernet)
 * ═══════════════════════════════════════════════════════════════ */
static void run_mesh(const Config& c, bool l2bridge) {
    bool tap = l2bridge;
    int tun_fd = tuntap_open(c.iface, tap);
    if (tun_fd < 0) {
        fprintf(stderr,"[datalink] Cannot open %s — try running as root\n",
                tap?"TAP":"TUN");
        return;
    }

    IIOCtx io;
    if (!iio_setup(io, c, true, true)) { close(tun_fd); return; }

    DatalinkStats     stats;
    stats.cur_mod = c.mod_code;
    std::atomic<int> cur_mod{c.mod_code};
    FrameRing<64> tx_ring;
    auto t0 = std::chrono::steady_clock::now();
    auto t_stats = t0;

    /* TX thread: reads from tx_ring, sends over radio */
    std::thread tx_thr(tx_frames, std::ref(io), std::ref(tx_ring),
                       std::cref(c), std::ref(stats), std::ref(cur_mod));

    /* RX thread: receives from radio, writes to TUN/TAP */
    auto on_frame = [&](const uint8_t* data, int len, uint8_t /*flags*/) {
        if (write(tun_fd, data, len) < 0) { /* ignore EAGAIN */ }
        stats.bytes_rx.fetch_add(len);
    };
    std::thread rx_thr(rx_frames, std::ref(io), std::cref(c),
                       std::ref(stats), std::ref(cur_mod), on_frame);

    /* Main: read TUN/TAP → tx_ring (with select timeout) */
    uint8_t pkt[MAX_PAYLOAD];
    fprintf(stderr,"[datalink] %s mesh running | TX %.3f MHz | RX %.3f MHz | BW %.2f MHz\n",
            tap?"L2Bridge":"IP-Mesh", c.freq_tx, c.freq_rx, bw_mhz(c.bw_code));

    while (!g_stop.load()) {
        fd_set fds; FD_ZERO(&fds); FD_SET(tun_fd, &fds);
        struct timeval tv{0, 100000}; /* 100 ms */
        int r = select(tun_fd+1, &fds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(tun_fd, &fds)) {
            int len = (int)read(tun_fd, pkt, sizeof(pkt));
            if (len > 0)
                while (!tx_ring.push(pkt, len) && !g_stop.load())
                    std::this_thread::yield();
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t_stats).count();
        if (dt >= 2.0) {
            double up  = std::chrono::duration<double>(now - t0).count();
            float txk  = (float)(stats.bytes_tx.exchange(0)*8.0/dt/1000.0);
            float rxk  = (float)(stats.bytes_rx.exchange(0)*8.0/dt/1000.0);
            /* Adaptive modulation feedback */
            if (c.adaptive) {
                int nm = AdaptMod::snr_to_code(stats.snr_db, cur_mod.load());
                if (nm != cur_mod.load()) cur_mod.store(nm);
            }
            stats.cur_mod = cur_mod.load();
            write_stats(c, stats, up, txk, rxk);
            fprintf(stderr,"\r[datalink] TX %.1f kbps | RX %.1f kbps | RSSI %.1f dBm | SNR %.1f dB | %s   ",
                    txk, rxk, stats.rssi_db, stats.snr_db,
                    AdaptMod::name(cur_mod.load()));
            t_stats = now;
        }
    }
    g_stop.store(true);
    tx_thr.join(); rx_thr.join();
    close(tun_fd);
    if (io.tx_buf) iio_buffer_destroy(io.tx_buf);
    if (io.rx_buf) iio_buffer_destroy(io.rx_buf);
    iio_context_destroy(io.ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * Arg parsing
 * ═══════════════════════════════════════════════════════════════ */
static int bw_str_to_code(const char* s) {
    double v = atof(s);
    if (v <= 1.25)  return BW_1P25MHZ;
    if (v <= 2.5)   return BW_2P5MHZ;
    if (v <= 5.0)   return BW_5MHZ;
    if (v <= 10.0)  return BW_10MHZ;
    return BW_20MHZ;
}
static int mod_str_to_code(const char* s) {
    if (!strcmp(s,"BPSK"))  return MOD_BPSK;
    if (!strcmp(s,"16QAM")) return MOD_16QAM;
    if (!strcmp(s,"64QAM")) return MOD_64QAM;
    if (!strcmp(s,"AUTO"))  return MOD_QPSK; /* start at QPSK, adapt */
    return MOD_QPSK;
}
static bool hex_to_bytes(const char* h, uint8_t* o, int n) {
    if ((int)strlen(h) < n*2) return false;
    for (int i=0;i<n;i++) { unsigned v; if(sscanf(h+i*2,"%02x",&v)!=1) return false; o[i]=(uint8_t)v; }
    return true;
}

static void parse_args(int argc, char** argv, Config& c) {
    static struct option opts[] = {
        {"mode",       required_argument,0,'M'},
        {"freq",       required_argument,0,'f'},
        {"freq-tx",    required_argument,0,'F'},
        {"freq-rx",    required_argument,0,'R'},
        {"bw",         required_argument,0,'b'},
        {"mod",        required_argument,0,'m'},
        {"atten",      required_argument,0,'a'},
        {"pluto",      required_argument,0,'p'},
        {"encrypt",    no_argument,      0,'e'},
        {"key",        required_argument,0,'k'},
        {"adaptive",   no_argument,      0,'A'},
        {"iface",      required_argument,0,'i'},
        {"tx-power",   required_argument,0,'P'},
        {"tx-gain",    required_argument,0,'G'},
        {"rx-gain",    required_argument,0,'g'},
        {"scan-start", required_argument,0,'s'},
        {"scan-step",  required_argument,0,'S'},
        {"scan-n",     required_argument,0,'n'},
        {0,0,0,0}
    };
    int opt;
    while ((opt=getopt_long(argc,argv,"",opts,nullptr))!=-1) switch(opt) {
    case 'M':
        if (!strcmp(optarg,"mesh"))      c.mode=Mode::MESH;
        else if (!strcmp(optarg,"p2p-tx"))c.mode=Mode::P2P_TX;
        else if (!strcmp(optarg,"p2p-rx"))c.mode=Mode::P2P_RX;
        else if (!strcmp(optarg,"l2bridge"))c.mode=Mode::L2BRIDGE;
        else if (!strcmp(optarg,"p2mp")) c.mode=Mode::P2MP;
        else if (!strcmp(optarg,"scan")) c.mode=Mode::SCAN;
        else if (!strcmp(optarg,"ranging"))c.mode=Mode::RANGING;
        break;
    case 'f': c.freq_tx=c.freq_rx=atof(optarg); break;
    case 'F': c.freq_tx=atof(optarg); break;
    case 'R': c.freq_rx=atof(optarg); break;
    case 'b': c.bw_code=bw_str_to_code(optarg); break;
    case 'm': c.mod_code=mod_str_to_code(optarg);
              c.adaptive=!strcmp(optarg,"AUTO"); break;
    case 'a': c.tx_atten=atoi(optarg);
              c.tx_power_dbm=0.0f-(float)c.tx_atten; break;
    case 'p': c.pluto_ip=optarg; break;
    case 'e': c.encrypt=true; break;
    case 'k': hex_to_bytes(optarg,c.aes_key,32); break;
    case 'A': c.adaptive=true; break;
    case 'i': c.iface=optarg; break;
    case 'P': c.tx_power_dbm=atof(optarg);
              c.tx_atten=(int)(0.0f-c.tx_power_dbm); break;
    case 'G': c.g_tx=atof(optarg); break;
    case 'g': c.g_rx=atof(optarg); break;
    case 's': c.scan_start=atof(optarg); break;
    case 'S': c.scan_step=atof(optarg); break;
    case 'n': c.scan_n=atoi(optarg); break;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * main()
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char** argv) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGUSR1, on_usr1);
    crc_init();

    /* Node ID = lower 32 bits of hostname hash */
    char hn[64]=""; gethostname(hn,sizeof(hn));
    uint32_t hid=0; for(int i=0;hn[i];i++) hid=hid*31+(uint8_t)hn[i];
    g_node_id = hid;

    Config c;
    parse_args(argc, argv, c);
    c.tx_power_dbm = 0.0f - (float)c.tx_atten;

    fprintf(stderr,
            "[datalink] mode=%-10s  freq_tx=%.3f freq_rx=%.3f MHz  bw=%.2f MHz  "
            "mod=%s%s  node=%08X\n",
            (c.mode==Mode::MESH?"mesh":c.mode==Mode::L2BRIDGE?"l2bridge":
             c.mode==Mode::P2P_TX?"p2p-tx":c.mode==Mode::P2P_RX?"p2p-rx":
             c.mode==Mode::SCAN?"scan":"ranging"),
            c.freq_tx, c.freq_rx, bw_mhz(c.bw_code),
            AdaptMod::name(c.mod_code), c.adaptive?"+AUTO":"",
            g_node_id);

    switch (c.mode) {
    case Mode::P2P_TX:  run_p2p_tx(c); break;
    case Mode::P2P_RX:  run_p2p_rx(c); break;
    case Mode::MESH:    run_mesh(c, false); break;
    case Mode::L2BRIDGE:run_mesh(c, true);  break;
    case Mode::P2MP:    run_p2p_tx(c); break;   /* broadcast TX = same as p2p-tx */
    case Mode::SCAN:    run_scan(c); break;
    case Mode::RANGING:
        /* Ranging: just run RX, print RSSI → distance every second */
        {
            IIOCtx io; iio_setup(io, c, false, true);
            DatalinkStats stats; stats.cur_mod=c.mod_code;
            std::atomic<int> cur_mod{c.mod_code};
            auto on_frame=[](const uint8_t*,int,uint8_t){};
            std::thread rx_thr(rx_frames,std::ref(io),std::cref(c),
                               std::ref(stats),std::ref(cur_mod),on_frame);
            while(!g_stop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                float d=rssi_to_dist_km(stats.rssi_db,(float)c.freq_rx,
                                        c.tx_power_dbm,c.g_tx,c.g_rx);
                printf("RSSI=%.1f dBm  SNR=%.1f dB  Distance=%.3f km\n",
                       stats.rssi_db,stats.snr_db,d);
                fflush(stdout);
            }
            g_stop.store(true); rx_thr.join();
            if(io.rx_buf)iio_buffer_destroy(io.rx_buf);
            iio_context_destroy(io.ctx);
        }
        break;
    }
    fprintf(stderr,"\n[datalink] Done.\n");
    return 0;
}
