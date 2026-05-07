/*
 * link_tx.cpp  —  SDR Data Link Transmitter  (professional multi-threaded)
 *
 * Architecture:
 *   [Input thread: stdin/UDP]  →  [SPSC frame queue]  →  [Modulation thread: IQ→PlutoSDR]
 *
 * Usage:
 *   ./link_tx [options]
 *   cat data.bin | ./link_tx --freq 434.0 --mod QPSK
 *
 * Options:
 *   --freq   434.0          Center frequency MHz
 *   --rate   20             Sample rate MSPS
 *   --mod    QPSK           BPSK | QPSK | QAM16 | QAM64
 *   --atten  10             TX attenuation dB (0=max power)
 *   --pluto  192.168.2.1    PlutoSDR IP
 *   --source stdin|udp      Data source
 *   --port   5005           UDP listen port
 *   --encrypt               AES-256-CTR per-frame encryption
 *   --key    <hex64>        32-byte key as 64 hex chars
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
#include <getopt.h>
#include <math.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

/* ── Frame constants ──────────────────────────────────────── */
#define SYNC_WORD    0xDEADBEEFU
#define MAX_PAYLOAD  512
#define SPS          4
#define ROLLOFF      0.35f
#define RRC_TAPS     32

/* ═══════════════════════════════════════════════════════════
 * Lock-free SPSC frame queue
 * Input thread produces raw payload bytes.
 * Modulation thread consumes, encodes, modulates, pushes to IIO.
 * ══════════════════════════════════════════════════════════ */
template<size_t SLOTS>
class FrameQueue {
    static_assert((SLOTS & (SLOTS-1)) == 0, "must be power of 2");
    static constexpr size_t MASK = SLOTS - 1;

    struct alignas(64) Slot {
        uint8_t data[MAX_PAYLOAD];
        int     len = 0;
    };

    Slot slots_[SLOTS];
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};

public:
    bool push(const uint8_t* buf, int len) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == SLOTS) return false;
        auto& s = slots_[h & MASK];
        memcpy(s.data, buf, len);
        s.len = len;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    Slot* peek() {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return nullptr;
        return &slots_[t & MASK];
    }

    void consume() { tail_.fetch_add(1, std::memory_order_release); }

    size_t size() const {
        return (size_t)(head_.load(std::memory_order_relaxed)
                      - tail_.load(std::memory_order_relaxed));
    }
};

/* ═══════════════════════════════════════════════════════════
 * Config
 * ══════════════════════════════════════════════════════════ */
struct Config {
    double    freq_mhz    = 434.0;
    long long rate_sps    = 20000000LL;
    long long rf_bw       = 18000000LL;
    int       tx_atten    = 10;
    char      pluto_ip[32]= "192.168.2.1";
    char      mod_str[16] = "QPSK";
    bool      use_udp     = false;
    int       udp_port    = 5005;
    bool      encrypt     = false;
    uint8_t   aes_key[32] = {};
};

static std::atomic<bool> g_stop{false};
static void on_sig(int) { g_stop.store(true); }

/* Runtime TX power + frequency control via SIGUSR1.
 * Server writes to /tmp/sdr_tx_ctrl:  atten_db freq_mhz
 *   e.g.  "20 434.000"
 * Then sends SIGUSR1.  Main loop applies both changes to hardware. */
static std::atomic<int>   g_new_atten{-1};       /* -1 = no change */
static std::atomic<float> g_new_freq_mhz{-1.0f}; /* -1 = no change */
static iio_channel*       g_txch  = nullptr;
static iio_channel*       g_lo_tx = nullptr;
static void on_usr1(int) {
    FILE* f = fopen("/tmp/sdr_tx_ctrl", "r");
    if (!f) return;
    int   atten = -1;
    float freq  = -1.0f;
    int _r = fscanf(f, "%d %f", &atten, &freq); (void)_r;
    fclose(f);
    if (atten >= 0 && atten <= 89)
        g_new_atten.store(atten, std::memory_order_relaxed);
    if (freq > 0.0f)
        g_new_freq_mhz.store(freq, std::memory_order_relaxed);
}

/* ── CRC-32 ───────────────────────────────────────────────── */
static uint32_t crc_tab[256];
static void crc_init(){
    for(int i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c=(c&1)?(0xEDB88320U^(c>>1)):(c>>1);
        crc_tab[i]=c;
    }
}
static uint32_t crc32(const uint8_t*d,size_t n){
    uint32_t c=0xFFFFFFFFU;
    for(size_t i=0;i<n;i++) c=crc_tab[(c^d[i])&0xFF]^(c>>8);
    return c^0xFFFFFFFFU;
}

/* ── AES-256-CTR ──────────────────────────────────────────── */
static void aes_ctr(const uint8_t*key,uint64_t ctr,
                    const uint8_t*in,uint8_t*out,int len){
    uint8_t iv[16]={};
    memcpy(iv+8,&ctr,8);
    EVP_CIPHER_CTX*ctx=EVP_CIPHER_CTX_new();
    int outl=0;
    EVP_EncryptInit_ex(ctx,EVP_aes_256_ctr(),NULL,key,iv);
    EVP_EncryptUpdate(ctx,out,&outl,in,len);
    EVP_CIPHER_CTX_free(ctx);
}

/* ── Modem helpers ────────────────────────────────────────── */
static modulation_scheme str_to_scheme(const char*s){
    if(!strcmp(s,"BPSK"))  return LIQUID_MODEM_BPSK;
    if(!strcmp(s,"QPSK"))  return LIQUID_MODEM_QPSK;
    if(!strcmp(s,"QAM16")) return LIQUID_MODEM_QAM16;
    if(!strcmp(s,"QAM64")) return LIQUID_MODEM_QAM64;
    return LIQUID_MODEM_QPSK;
}
static int scheme_bps(modulation_scheme s){
    switch(s){case LIQUID_MODEM_BPSK:return 1;case LIQUID_MODEM_QPSK:return 2;
              case LIQUID_MODEM_QAM16:return 4;case LIQUID_MODEM_QAM64:return 6;default:return 2;}
}

/* ── Build frame ──────────────────────────────────────────── */
static int build_frame(const uint8_t*payload,int plen,uint8_t*out,
                       bool enc,const uint8_t*key,uint64_t ctr){
    uint8_t enc_pl[MAX_PAYLOAD];
    if(enc){ aes_ctr(key,ctr,payload,enc_pl,plen); payload=enc_pl; }
    int pos=0;
    out[0]=(SYNC_WORD>>24)&0xFF; out[1]=(SYNC_WORD>>16)&0xFF;
    out[2]=(SYNC_WORD>> 8)&0xFF; out[3]=(SYNC_WORD    )&0xFF; pos=4;
    out[pos]=(uint8_t)(plen>>8); out[pos+1]=(uint8_t)plen; pos+=2;
    memcpy(out+pos,payload,plen); pos+=plen;
    uint32_t crc=crc32(out,pos); memcpy(out+pos,&crc,4); pos+=4;
    return pos;
}

/* ── Stats ────────────────────────────────────────────────── */
struct alignas(64) Stats {
    std::atomic<uint64_t> frames_sent{0};
    std::atomic<uint64_t> push_errors{0};
    std::atomic<int64_t>  bytes_in{0};
};

static void write_stats_file(const Config&c,const Stats&st,double uptime,float kbps){
    FILE*f=fopen("/tmp/sdr_link_stats.json","w");
    if(!f) return;
    /* PlutoSDR AD9363 max TX ≈ 0 dBm at SMA; attenuation reduces from there */
    float tx_dbm = 0.0f - (float)c.tx_atten;
    float tx_mw  = powf(10.0f, tx_dbm / 10.0f);
    fprintf(f,
        "{\"mode\":\"tx\","
        "\"freq_mhz\":%.3f,\"mod\":\"%s\",\"rate_msps\":%lld,"
        "\"tx_atten_db\":%d,\"encrypt\":%s,"
        "\"frames_good\":%llu,\"frames_bad\":%llu,"
        "\"throughput_kbps\":%.1f,"
        "\"rssi_db\":0,\"snr_db\":0,"
        "\"tx_power_dbm\":%.1f,\"tx_power_mw\":%.4f,"
        "\"rx_power_mw\":0,"
        "\"uptime_s\":%.0f,\"status\":\"running\"}\n",
        c.freq_mhz,c.mod_str,c.rate_sps/1000000LL,c.tx_atten,
        c.encrypt?"true":"false",
        (unsigned long long)st.frames_sent.load(),
        (unsigned long long)st.push_errors.load(),
        kbps, tx_dbm, tx_mw, uptime);
    fclose(f);
}

/* ── Arg parsing ──────────────────────────────────────────── */
static bool hex_to_bytes(const char*h,uint8_t*o,int n){
    if((int)strlen(h)<n*2) return false;
    for(int i=0;i<n;i++){unsigned v;if(sscanf(h+i*2,"%02x",&v)!=1)return false;o[i]=(uint8_t)v;}
    return true;
}
static void parse_args(int argc,char**argv,Config&cfg){
    static struct option opts[]={
        {"freq",  required_argument,0,'f'},{"rate",   required_argument,0,'r'},
        {"mod",   required_argument,0,'m'},{"atten",  required_argument,0,'a'},
        {"pluto", required_argument,0,'p'},{"source", required_argument,0,'s'},
        {"port",  required_argument,0,'u'},{"encrypt",no_argument,      0,'e'},
        {"key",   required_argument,0,'k'},{0,0,0,0}
    };
    int c,idx=0;
    while((c=getopt_long(argc,argv,"f:r:m:a:p:s:u:ek:",opts,&idx))!=-1){
        switch(c){
        case 'f': cfg.freq_mhz=atof(optarg); break;
        case 'r': cfg.rate_sps=(long long)(atof(optarg)*1e6); break;
        case 'm': snprintf(cfg.mod_str,sizeof(cfg.mod_str),"%s",optarg); break;
        case 'a': cfg.tx_atten=atoi(optarg); break;
        case 'p': snprintf(cfg.pluto_ip,sizeof(cfg.pluto_ip),"%s",optarg); break;
        case 's': cfg.use_udp=(!strcmp(optarg,"udp")); break;
        case 'u': cfg.udp_port=atoi(optarg); break;
        case 'e': cfg.encrypt=true; break;
        case 'k': if(!hex_to_bytes(optarg,cfg.aes_key,32)) fprintf(stderr,"Bad key\n"); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Input thread  —  reads data, pushes raw frames to queue
 * ══════════════════════════════════════════════════════════ */
static void input_thread(const Config& cfg,
                         FrameQueue<64>& queue,
                         Stats& stats,
                         std::atomic<bool>& stop) {
    int udp_fd = -1;
    struct sockaddr_in udp_src = {};
    if (cfg.use_udp) {
        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        udp_src.sin_family      = AF_INET;
        udp_src.sin_addr.s_addr = INADDR_ANY;
        udp_src.sin_port        = htons(cfg.udp_port);
        if (bind(udp_fd, (struct sockaddr*)&udp_src, sizeof(udp_src)) < 0) {
            perror("[tx] UDP bind");
            stop.store(true);
            return;
        }
        fprintf(stderr, "[tx] UDP source: port %d\n", cfg.udp_port);
    }

    uint8_t buf[MAX_PAYLOAD];
    while (!stop.load(std::memory_order_relaxed)) {
        int len = 0;
        if (cfg.use_udp) {
            socklen_t sl = sizeof(udp_src);
            len = (int)recvfrom(udp_fd, buf, MAX_PAYLOAD, 0,
                                (struct sockaddr*)&udp_src, &sl);
            if (len <= 0) continue;
        } else {
            len = (int)fread(buf, 1, MAX_PAYLOAD, stdin);
            if (len == 0) { stop.store(true); break; }
        }

        /* Spin until there's space in the queue */
        while (!queue.push(buf, len) && !stop.load(std::memory_order_relaxed))
            std::this_thread::yield();

        stats.bytes_in.fetch_add(len, std::memory_order_relaxed);
    }

    if (udp_fd >= 0) close(udp_fd);
}

/* ═══════════════════════════════════════════════════════════
 * main()  —  runs the modulation loop (IQ → PlutoSDR TX)
 * ══════════════════════════════════════════════════════════ */
int main(int argc, char** argv) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGUSR1, on_usr1);
    crc_init();

    Config cfg;
    parse_args(argc, argv, cfg);

    modulation_scheme scheme = str_to_scheme(cfg.mod_str);
    int bps = scheme_bps(scheme);

    fprintf(stderr,
            "[tx] %.3f MHz | %lld MSPS | %s (%d bps/sym) | atten=%d dB%s\n",
            cfg.freq_mhz, cfg.rate_sps/1000000LL, cfg.mod_str, bps,
            cfg.tx_atten, cfg.encrypt?" | AES-256":"");

    /* ── libiio ────────────────────────────────────────────── */
    iio_context* ctx = iio_create_network_context(cfg.pluto_ip);
    if (!ctx) { fprintf(stderr, "[tx] Cannot connect to %s\n", cfg.pluto_ip); return 1; }

    iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
    iio_device* tx  = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    if (!phy || !tx) { fprintf(stderr, "[tx] Devices not found\n"); return 1; }

    g_lo_tx = iio_device_find_channel(phy, "altvoltage1", true);
    iio_channel_attr_write_longlong(g_lo_tx, "frequency", (long long)(cfg.freq_mhz * 1e6));

    g_txch = iio_device_find_channel(phy, "voltage0", true);
    iio_channel_attr_write_longlong(g_txch, "sampling_frequency", cfg.rate_sps);
    iio_channel_attr_write_longlong(g_txch, "rf_bandwidth",       cfg.rf_bw);
    iio_channel_attr_write_longlong(g_txch, "hardwaregain",       -cfg.tx_atten);

    iio_channel_enable(iio_device_find_channel(tx, "voltage0", true));
    iio_channel_enable(iio_device_find_channel(tx, "voltage1", true));
    /* local aliases so loop doesn't use globals directly */
    iio_channel* txch  = g_txch;
    iio_channel* lo_tx = g_lo_tx;

    /* IIO buffer sized for one full frame at any modulation */
    const int MAX_FRAME_SAMPS = (MAX_PAYLOAD + 10) * 8 / bps * SPS;
    iio_buffer* iio_buf = iio_device_create_buffer(tx, MAX_FRAME_SAMPS * 4, false);
    if (!iio_buf) { fprintf(stderr, "[tx] IIO buffer failed\n"); return 1; }

    /* ── liquid-dsp ────────────────────────────────────────── */
    modemcf        mod    = modemcf_create(scheme);
    firinterp_crcf interp = firinterp_crcf_create_prototype(
                               LIQUID_FIRFILT_RRC, SPS, RRC_TAPS, ROLLOFF, 0);

    std::vector<int16_t> iq_stage;
    iq_stage.reserve(MAX_FRAME_SAMPS * 2 * 4);
    liquid_float_complex iq_out[SPS];

    /* Frame state */
    uint64_t frame_ctr = 0;
    uint8_t  frame_buf[MAX_PAYLOAD + 10];
    Stats    stats;

    /* Timing */
    auto t0      = std::chrono::steady_clock::now();
    auto t_stats = t0;

    /* ── Launch input thread ────────────────────────────────── */
    FrameQueue<64> frame_queue;
    std::thread in_thread(input_thread,
                          std::cref(cfg), std::ref(frame_queue),
                          std::ref(stats), std::ref(g_stop));

    fprintf(stderr, "[tx] Running — input thread started, modulation in main thread\n");

    /* ══════════════════════════════════════════════════════════
     * Modulation loop
     * ════════════════════════════════════════════════════════ */
    while (!g_stop.load(std::memory_order_relaxed)) {
        auto* slot = frame_queue.peek();
        if (!slot) {
            std::this_thread::yield();
            continue;
        }

        /* Build frame (with optional encryption) */
        int flen = build_frame(slot->data, slot->len, frame_buf,
                               cfg.encrypt, cfg.aes_key, frame_ctr++);
        frame_queue.consume();

        /* Modulate frame bytes → IQ samples */
        int total_bits = flen * 8;
        int n_sym      = (total_bits + bps - 1) / bps;
        iq_stage.clear();

        for (int sym = 0; sym < n_sym; sym++) {
            unsigned int s = 0;
            for (int b = 0; b < bps; b++) {
                int bit = sym * bps + b;
                int byte_i = bit / 8, bit_off = 7 - (bit % 8);
                s = (s << 1) | ((byte_i < flen) ? ((frame_buf[byte_i] >> bit_off) & 1) : 0);
            }

            liquid_float_complex sym_iq;
            modemcf_modulate(mod, s, &sym_iq);
            firinterp_crcf_execute(interp, sym_iq, iq_out);

            for (int k = 0; k < SPS; k++) {
                iq_stage.push_back((int16_t)(iq_out[k].real * 2047.0f));
                iq_stage.push_back((int16_t)(iq_out[k].imag * 2047.0f));
            }
        }

        /* Copy to IIO buffer and push */
        int16_t* dst = (int16_t*)iio_buffer_start(iio_buf);
        int16_t* end = (int16_t*)iio_buffer_end(iio_buf);
        int cap  = (int)(end - dst);
        int copy = (int)iq_stage.size() < cap ? (int)iq_stage.size() : cap;
        memcpy(dst, iq_stage.data(), copy * sizeof(int16_t));
        if (copy < cap) memset(dst + copy, 0, (cap - copy) * sizeof(int16_t));

        ssize_t pushed = iio_buffer_push(iio_buf);
        if (pushed < 0) {
            stats.push_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats.frames_sent.fetch_add(1, std::memory_order_relaxed);
        }

        /* Runtime power/frequency update (applied after each frame) */
        {
            int new_a = g_new_atten.exchange(-1, std::memory_order_relaxed);
            if (new_a >= 0 && txch) {
                iio_channel_attr_write_longlong(txch, "hardwaregain", -new_a);
                cfg.tx_atten = new_a;
                fprintf(stderr, "\n[tx] Power updated: atten=%d dB (%.1f dBm)\n",
                        new_a, 0.0f - new_a);
            }
            float new_f = g_new_freq_mhz.exchange(-1.0f, std::memory_order_relaxed);
            if (new_f > 0.0f && lo_tx) {
                iio_channel_attr_write_longlong(lo_tx, "frequency",
                                               (long long)(new_f * 1e6));
                cfg.freq_mhz = new_f;
                fprintf(stderr, "\n[tx] Frequency updated: %.3f MHz\n", new_f);
            }
        }

        /* Stats every 2 s */
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t_stats).count();
        if (dt >= 2.0) {
            int64_t b  = stats.bytes_in.exchange(0, std::memory_order_relaxed);
            float kbps = (float)(b * 8.0 / dt / 1000.0);
            double up  = std::chrono::duration<double>(now - t0).count();
            write_stats_file(cfg, stats, up, kbps);
            fprintf(stderr,
                    "\r[tx] sent=%-7llu errors=%-4llu  %.1f kbps  queue=%zu   ",
                    (unsigned long long)stats.frames_sent.load(),
                    (unsigned long long)stats.push_errors.load(),
                    kbps, frame_queue.size());
            t_stats = now;
        }
    }

    /* ── Teardown ──────────────────────────────────────────── */
    g_stop.store(true);
    in_thread.join();

    firinterp_crcf_destroy(interp);
    modemcf_destroy(mod);
    iio_buffer_destroy(iio_buf);
    iio_context_destroy(ctx);

    fprintf(stderr, "\n[tx] Stopped.  sent=%llu  errors=%llu\n",
            (unsigned long long)stats.frames_sent.load(),
            (unsigned long long)stats.push_errors.load());
    return 0;
}
