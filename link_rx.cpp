/*
 * link_rx.cpp  —  SDR Data Link Receiver  (professional multi-threaded)
 *
 * Architecture:
 *   [IIO capture thread]  →  [SPSC ring buffer]  →  [DSP thread]  →  stdout/UDP
 *   Separating capture from DSP prevents dropped samples when DSP stalls.
 *
 * Usage:
 *   ./link_rx [options] > received.bin
 *   ./link_rx --freq 434.0 --mod QPSK | pv > /dev/null
 *
 * Options:
 *   --freq   434.0          Center frequency MHz
 *   --rate   20             Sample rate MSPS (1–20)
 *   --mod    QPSK           BPSK | QPSK | QAM16 | QAM64
 *   --pluto  192.168.2.1    PlutoSDR IP
 *   --sink   stdout|udp     Data sink
 *   --sink-ip 127.0.0.1     UDP destination IP
 *   --port   5006           UDP destination port
 *   --encrypt               AES-256-CTR decryption
 *   --key    <hex64>        32-byte key as 64 hex chars
 *   --gain   fast_attack    gain_control_mode: manual|slow_attack|fast_attack
 *   --buffers 8             Number of ring-buffer slots (power of 2)
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
#include <memory>
#include <chrono>

/* ── Constants (must match link_tx.cpp) ──────────────────── */
#define SYNC_WORD    0xDEADBEEFU
#define MAX_PAYLOAD  512
#define SPS          4
#define ROLLOFF      0.35f
#define RRC_TAPS     32
#define IIO_BUF_SAMPS (256*1024)

/* ═══════════════════════════════════════════════════════════
 * Lock-free SPSC ring buffer
 * Producer writes IQ sample blocks; consumer runs DSP on them.
 * Uses cache-line padding to prevent false sharing.
 * ══════════════════════════════════════════════════════════ */
template<size_t SLOTS>
class IQQueue {
    static_assert((SLOTS & (SLOTS-1)) == 0, "SLOTS must be power of 2");
    static constexpr size_t MASK = SLOTS - 1;

    struct alignas(64) Slot {
        std::vector<int16_t> data;  /* interleaved I/Q int16 pairs */
    };

    Slot slots_[SLOTS];
    alignas(64) std::atomic<uint64_t> head_{0};  /* writer index */
    alignas(64) std::atomic<uint64_t> tail_{0};  /* reader index */

public:
    /* Push: called by capture thread only. Returns false if full (drop). */
    bool push(const int16_t* p, size_t n_iq_pairs) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == SLOTS)
            return false;
        auto& s = slots_[h & MASK];
        s.data.assign(p, p + n_iq_pairs * 2);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /* Try to borrow the next slot without copying. Returns nullptr if empty. */
    Slot* peek() {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return nullptr;
        return &slots_[t & MASK];
    }

    /* Release the previously peeked slot. */
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
    char      pluto_ip[32]= "192.168.2.1";
    char      mod_str[16] = "QPSK";
    char      gain_mode[32]="fast_attack";
    bool      use_udp     = false;
    char      sink_ip[32] = "127.0.0.1";
    int       udp_port    = 5006;
    bool      encrypt     = false;
    uint8_t   aes_key[32] = {};
    int       n_slots     = 16;
};

/* ═══════════════════════════════════════════════════════════
 * Global stop flag (set by SIGINT / SIGTERM)
 * ══════════════════════════════════════════════════════════ */
static std::atomic<bool>  g_stop{false};
static void on_sig(int) { g_stop.store(true); }

/* Runtime frequency tuning via SIGUSR1.
 * Server writes new freq (MHz) to /tmp/sdr_rx_ctrl, then sends SIGUSR1. */
static std::atomic<float> g_new_freq_mhz{-1.0f};
static iio_channel*       g_lo_rx = nullptr;
static void on_usr1(int) {
    FILE* f = fopen("/tmp/sdr_rx_ctrl", "r");
    if (!f) return;
    float freq = -1.0f;
    int _r = fscanf(f, "%f", &freq); (void)_r;
    fclose(f);
    if (freq > 0.0f)
        g_new_freq_mhz.store(freq, std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════
 * CRC-32 (IEEE 802.3)
 * ══════════════════════════════════════════════════════════ */
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

/* ═══════════════════════════════════════════════════════════
 * AES-256-CTR (OpenSSL) — symmetric, same code for enc/dec
 * ══════════════════════════════════════════════════════════ */
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

/* ═══════════════════════════════════════════════════════════
 * Modem helpers
 * ══════════════════════════════════════════════════════════ */
static modulation_scheme str_to_scheme(const char*s){
    if(!strcmp(s,"BPSK"))  return LIQUID_MODEM_BPSK;
    if(!strcmp(s,"QPSK"))  return LIQUID_MODEM_QPSK;
    if(!strcmp(s,"QAM16")) return LIQUID_MODEM_QAM16;
    if(!strcmp(s,"QAM64")) return LIQUID_MODEM_QAM64;
    fprintf(stderr,"[rx] Unknown mod '%s', defaulting to QPSK\n",s);
    return LIQUID_MODEM_QPSK;
}
static int scheme_bps(modulation_scheme s){
    switch(s){case LIQUID_MODEM_BPSK:return 1;case LIQUID_MODEM_QPSK:return 2;
              case LIQUID_MODEM_QAM16:return 4;case LIQUID_MODEM_QAM64:return 6;default:return 2;}
}

/* ═══════════════════════════════════════════════════════════
 * Stats (written by DSP thread, read by stats writer)
 * ══════════════════════════════════════════════════════════ */
struct alignas(64) Stats {
    std::atomic<uint64_t> frames_good{0};
    std::atomic<uint64_t> frames_bad{0};
    std::atomic<uint64_t> dropped_blocks{0};  /* IQ blocks dropped (queue full) */
    std::atomic<int64_t>  bytes_out{0};        /* since last stats write */
    float rssi_db  = 0.0f;
    float snr_db   = 0.0f;
};

static void write_stats_file(const Config&c,const Stats&st,double uptime,float kbps){
    FILE*f=fopen("/tmp/sdr_link_stats.json","w");
    if(!f) return;
    /* rssi_db from liquid-dsp AGC is relative to full-scale (dBFS).
       AD9363 RX gain range 0–73 dB; at typical 40 dB gain,
       0 dBFS ≈ -20 dBm input → rssi_db is a good relative indicator. */
    float rx_mw = powf(10.0f, st.rssi_db / 10.0f);
    fprintf(f,
        "{\"mode\":\"rx\","
        "\"freq_mhz\":%.3f,\"mod\":\"%s\",\"rate_msps\":%lld,"
        "\"encrypt\":%s,"
        "\"frames_good\":%llu,\"frames_bad\":%llu,"
        "\"dropped_blocks\":%llu,"
        "\"throughput_kbps\":%.1f,"
        "\"rssi_db\":%.1f,\"snr_db\":%.1f,"
        "\"tx_power_dbm\":0,\"tx_power_mw\":0,"
        "\"rx_power_mw\":%.6f,"
        "\"uptime_s\":%.0f,\"status\":\"running\"}\n",
        c.freq_mhz,c.mod_str,c.rate_sps/1000000LL,
        c.encrypt?"true":"false",
        (unsigned long long)st.frames_good.load(),
        (unsigned long long)st.frames_bad.load(),
        (unsigned long long)st.dropped_blocks.load(),
        kbps, st.rssi_db, st.snr_db,
        rx_mw, uptime);
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════
 * Frame decoder state machine
 * ══════════════════════════════════════════════════════════ */
struct Framer {
    enum class State { HUNT, LEN, PAYLOAD } state = State::HUNT;
    uint32_t shift  = 0;
    uint8_t  buf[MAX_PAYLOAD+10] = {};
    int      pos    = 0;
    int      expect = 0;
    uint16_t plen   = 0;

    /* Returns payload length if a complete valid frame was received, else 0. */
    int push_byte(uint8_t b, uint8_t* out,
                  bool enc, const uint8_t* key, uint64_t& frame_ctr) {
        switch(state){
        case State::HUNT:
            shift=(shift<<8)|b;
            if(shift==SYNC_WORD){
                buf[0]=(SYNC_WORD>>24)&0xFF; buf[1]=(SYNC_WORD>>16)&0xFF;
                buf[2]=(SYNC_WORD>> 8)&0xFF; buf[3]= SYNC_WORD     &0xFF;
                pos=4; state=State::LEN; expect=2;
            }
            break;
        case State::LEN:
            buf[pos++]=b;
            if(--expect==0){
                plen=((uint16_t)buf[4]<<8)|buf[5];
                if(plen==0||plen>MAX_PAYLOAD){ state=State::HUNT; shift=0; }
                else { expect=plen+4; state=State::PAYLOAD; }
            }
            break;
        case State::PAYLOAD:
            buf[pos++]=b;
            if(--expect==0){
                int hlen=6+plen;
                uint32_t got_crc,exp_crc;
                memcpy(&got_crc,buf+hlen,4);
                exp_crc=crc32(buf,hlen);
                state=State::HUNT; shift=0; pos=0;
                if(got_crc!=exp_crc) return -1;   /* CRC error */
                if(enc){
                    aes_ctr(key,frame_ctr,buf+6,out,plen);
                    frame_ctr++;
                } else {
                    memcpy(out,buf+6,plen);
                }
                return plen;
            }
            break;
        }
        return 0;
    }
};

/* ═══════════════════════════════════════════════════════════
 * Argument parsing
 * ══════════════════════════════════════════════════════════ */
static bool hex_to_bytes(const char*h,uint8_t*o,int n){
    if((int)strlen(h)<n*2) return false;
    for(int i=0;i<n;i++){unsigned v;if(sscanf(h+i*2,"%02x",&v)!=1)return false;o[i]=(uint8_t)v;}
    return true;
}

static void parse_args(int argc,char**argv,Config&cfg){
    static struct option opts[]={
        {"freq",    required_argument,0,'f'},{"rate",    required_argument,0,'r'},
        {"mod",     required_argument,0,'m'},{"pluto",   required_argument,0,'p'},
        {"sink",    required_argument,0,'s'},{"sink-ip", required_argument,0,'i'},
        {"port",    required_argument,0,'u'},{"encrypt", no_argument,      0,'e'},
        {"key",     required_argument,0,'k'},{"gain",    required_argument,0,'g'},
        {"buffers", required_argument,0,'b'},{0,0,0,0}
    };
    int c,idx=0;
    while((c=getopt_long(argc,argv,"f:r:m:p:s:i:u:ek:g:b:",opts,&idx))!=-1){
        switch(c){
        case 'f': cfg.freq_mhz=atof(optarg); break;
        case 'r': cfg.rate_sps=(long long)(atof(optarg)*1e6); break;
        case 'm': snprintf(cfg.mod_str,sizeof(cfg.mod_str),"%s",optarg); break;
        case 'p': snprintf(cfg.pluto_ip,sizeof(cfg.pluto_ip),"%s",optarg); break;
        case 's': cfg.use_udp=(!strcmp(optarg,"udp")); break;
        case 'i': snprintf(cfg.sink_ip,sizeof(cfg.sink_ip),"%s",optarg); break;
        case 'u': cfg.udp_port=atoi(optarg); break;
        case 'e': cfg.encrypt=true; break;
        case 'k': if(!hex_to_bytes(optarg,cfg.aes_key,32)) fprintf(stderr,"Bad key\n"); break;
        case 'g': snprintf(cfg.gain_mode,sizeof(cfg.gain_mode),"%s",optarg); break;
        case 'b': cfg.n_slots=atoi(optarg); break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * IIO capture thread
 *   Calls iio_buffer_refill() in a tight loop.
 *   Pushes raw int16 IQ blocks to the ring queue.
 *   Never does DSP — stays at realtime priority.
 * ══════════════════════════════════════════════════════════ */
static void capture_thread(iio_buffer* buf, IQQueue<64>& queue,
                            Stats& stats, std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_relaxed)) {
        ssize_t nb = iio_buffer_refill(buf);
        if (nb < 0) { stop.store(true); break; }

        const int16_t* p   = (const int16_t*)iio_buffer_start(buf);
        const int16_t* end = (const int16_t*)iio_buffer_end(buf);
        size_t n_pairs = (size_t)(end - p) / 2;

        if (!queue.push(p, n_pairs))
            stats.dropped_blocks.fetch_add(1, std::memory_order_relaxed);
    }
}

/* ═══════════════════════════════════════════════════════════
 * main()
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
            "[rx] %.3f MHz | %lld MSPS | %s (%d bps/sym) | gain=%s%s\n",
            cfg.freq_mhz, cfg.rate_sps/1000000LL, cfg.mod_str, bps,
            cfg.gain_mode, cfg.encrypt?" | AES-256":"");

    /* ── UDP sink ──────────────────────────────────────────── */
    int udp_fd = -1;
    struct sockaddr_in udp_dst = {};
    if (cfg.use_udp) {
        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        udp_dst.sin_family      = AF_INET;
        udp_dst.sin_addr.s_addr = inet_addr(cfg.sink_ip);
        udp_dst.sin_port        = htons(cfg.udp_port);
        fprintf(stderr, "[rx] UDP sink: %s:%d\n", cfg.sink_ip, cfg.udp_port);
    }

    /* ── libiio setup ──────────────────────────────────────── */
    iio_context* ctx = iio_create_network_context(cfg.pluto_ip);
    if (!ctx) { fprintf(stderr, "[rx] Cannot connect to %s\n", cfg.pluto_ip); return 1; }

    iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
    iio_device* rx  = iio_context_find_device(ctx, "cf-ad9361-lpc");
    if (!phy || !rx) { fprintf(stderr, "[rx] Devices not found\n"); return 1; }

    g_lo_rx = iio_device_find_channel(phy, "altvoltage0", true);
    iio_channel_attr_write_longlong(g_lo_rx, "frequency", (long long)(cfg.freq_mhz * 1e6));
    iio_channel* lo = g_lo_rx;

    iio_channel* rxch = iio_device_find_channel(phy, "voltage0", false);
    iio_channel_attr_write_longlong(rxch, "sampling_frequency", cfg.rate_sps);
    iio_channel_attr_write_longlong(rxch, "rf_bandwidth",       cfg.rf_bw);
    iio_channel_attr_write(rxch, "gain_control_mode", cfg.gain_mode);

    iio_channel_enable(iio_device_find_channel(rx, "voltage0", false));
    iio_channel_enable(iio_device_find_channel(rx, "voltage1", false));

    iio_buffer* iio_buf = iio_device_create_buffer(rx, IIO_BUF_SAMPS, false);
    if (!iio_buf) { fprintf(stderr, "[rx] IIO buffer failed\n"); return 1; }

    /* ── Ring buffer between capture & DSP ─────────────────── */
    IQQueue<64> queue;
    Stats stats;

    /* ── DSP objects (all in this thread) ──────────────────── */
    agc_crcf      agc    = agc_crcf_create();
    agc_crcf_set_bandwidth(agc, 1e-3f);

    firdecim_crcf  decim = firdecim_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC, SPS, RRC_TAPS, ROLLOFF, 0);
    symsync_crcf   sync  = symsync_crcf_create_rnyquist(
                             LIQUID_FIRFILT_RRC, SPS, RRC_TAPS, ROLLOFF, 32);
    symsync_crcf_set_lf_bw(sync, 0.02f);

    /* Carrier recovery: nco_crcf with 2nd-order PLL (replaces costas_loop_crcf) */
    nco_crcf nco = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(nco, 0.04f);
    modemcf  demod = modemcf_create(scheme);
    Framer           framer;

    /* Intermediate state */
    liquid_float_complex decim_in[SPS];
    int     decim_idx  = 0;
    uint8_t cur_byte   = 0;
    int     bit_cursor = 0;
    uint64_t frame_ctr = 0;
    uint8_t  out_buf[MAX_PAYLOAD];

    /* Stats timing */
    auto t0         = std::chrono::steady_clock::now();
    auto t_stats    = t0;

    /* ── Launch capture thread ──────────────────────────────── */
    std::thread cap_thread(capture_thread,
                           iio_buf, std::ref(queue),
                           std::ref(stats), std::ref(g_stop));

    fprintf(stderr, "[rx] Running — capture thread started, DSP in main thread\n");

    /* ══════════════════════════════════════════════════════════
     * DSP loop (main thread)
     * Pops IQ blocks from the ring buffer and runs the full
     * demodulation chain: AGC → decim → timing → phase → demod
     * ════════════════════════════════════════════════════════ */
    while (!g_stop.load(std::memory_order_relaxed)) {
        auto* slot = queue.peek();
        if (!slot) {
            std::this_thread::yield();
            continue;
        }

        const int16_t* p   = slot->data.data();
        const int16_t* end = p + slot->data.size();

        while (p < end) {
            liquid_float_complex samp{*p / 2047.0f, *(p+1) / 2047.0f};
            p += 2;

            /* AGC — normalizes signal amplitude */
            liquid_float_complex agc_out;
            agc_crcf_execute(agc, samp, &agc_out);
            stats.rssi_db = 0.999f*stats.rssi_db + 0.001f*agc_crcf_get_rssi(agc);

            /* Accumulate SPS=4 samples before calling matched filter */
            decim_in[decim_idx++] = agc_out;
            if (decim_idx < SPS) continue;
            decim_idx = 0;

            liquid_float_complex decim_out;
            firdecim_crcf_execute(decim, decim_in, &decim_out);

            /* Symbol timing recovery (Gardner TED) */
            liquid_float_complex sync_out[8];
            unsigned int nsync = 0;
            symsync_crcf_execute(sync, &decim_out, 1, sync_out, &nsync);

            for (unsigned si = 0; si < nsync; si++) {
                /* Carrier phase/frequency recovery via NCO PLL */
                liquid_float_complex phase_out;
                nco_crcf_mix_down(nco, sync_out[si], &phase_out);
                nco_crcf_step(nco);

                /* QPSK/QAM symbol decision */
                unsigned int sym;
                modemcf_demodulate(demod, phase_out, &sym);

                /* Decision-directed phase error for PLL */
                float re = phase_out.real, im = phase_out.imag;
                float sr = (re >= 0.0f) ? 1.0f : -1.0f;
                float si_ = (im >= 0.0f) ? 1.0f : -1.0f;
                nco_crcf_pll_step(nco, sr * im - si_ * re);

                /* EVM → SNR estimate */
                float evm = modemcf_get_demodulator_evm(demod);
                if (evm > 1e-6f)
                    stats.snr_db = 0.999f*stats.snr_db
                                 + 0.001f*10.0f*log10f(1.0f/(evm*evm+1e-12f));

                /* Unpack bps bits, MSB first */
                for (int b = bps-1; b >= 0; b--) {
                    cur_byte = (uint8_t)((cur_byte << 1) | ((sym >> b) & 1));
                    if (++bit_cursor == 8) {
                        int plen = framer.push_byte(cur_byte, out_buf,
                                                    cfg.encrypt, cfg.aes_key,
                                                    frame_ctr);
                        if (plen > 0) {
                            if (cfg.use_udp && udp_fd >= 0) {
                                sendto(udp_fd, out_buf, plen, 0,
                                       (struct sockaddr*)&udp_dst,
                                       sizeof(udp_dst));
                            } else {
                                fwrite(out_buf, 1, plen, stdout);
                                fflush(stdout);
                            }
                            stats.bytes_out.fetch_add(plen, std::memory_order_relaxed);
                            stats.frames_good.fetch_add(1, std::memory_order_relaxed);
                        } else if (plen < 0) {
                            stats.frames_bad.fetch_add(1, std::memory_order_relaxed);
                        }
                        cur_byte = 0; bit_cursor = 0;
                    }
                }
            }
        }

        queue.consume();

        /* Runtime frequency update */
        {
            float new_f = g_new_freq_mhz.exchange(-1.0f, std::memory_order_relaxed);
            if (new_f > 0.0f && lo) {
                iio_channel_attr_write_longlong(lo, "frequency",
                                               (long long)(new_f * 1e6));
                cfg.freq_mhz = new_f;
                fprintf(stderr, "\n[rx] Frequency updated: %.3f MHz\n", new_f);
            }
        }

        /* Stats every 2 s */
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t_stats).count();
        if (dt >= 2.0) {
            int64_t b = stats.bytes_out.exchange(0, std::memory_order_relaxed);
            float kbps = (float)(b * 8.0 / dt / 1000.0);
            double up  = std::chrono::duration<double>(now - t0).count();
            write_stats_file(cfg, stats, up, kbps);
            fprintf(stderr,
                    "\r[rx] good=%-7llu bad=%-5llu drop=%-3llu "
                    "RSSI=%+6.1fdB SNR=%5.1fdB  %7.1f kbps  queue=%zu   ",
                    (unsigned long long)stats.frames_good.load(),
                    (unsigned long long)stats.frames_bad.load(),
                    (unsigned long long)stats.dropped_blocks.load(),
                    stats.rssi_db, stats.snr_db, kbps, queue.size());
            t_stats = now;
        }
    }

    /* ── Teardown ──────────────────────────────────────────── */
    g_stop.store(true);
    cap_thread.join();

    if (udp_fd >= 0) close(udp_fd);
    modemcf_destroy(demod);
    nco_crcf_destroy(nco);
    symsync_crcf_destroy(sync);
    firdecim_crcf_destroy(decim);
    agc_crcf_destroy(agc);
    iio_buffer_destroy(iio_buf);
    iio_context_destroy(ctx);

    fprintf(stderr,
            "\n[rx] Stopped.  good=%llu  bad=%llu  dropped_blocks=%llu\n",
            (unsigned long long)stats.frames_good.load(),
            (unsigned long long)stats.frames_bad.load(),
            (unsigned long long)stats.dropped_blocks.load());
    return 0;
}
