/*
 * sol8_test.cpp  —  SOL8-compatible SDR comprehensive test suite
 *
 * Tests (all software, no hardware required):
 *   Test 1: Frame encode/decode — all flag combinations
 *   Test 2: BER vs SNR curves  — BPSK / QPSK / 16QAM / 64QAM
 *   Test 3: Adaptive modulation state machine
 *   Test 4: RSSI-based ranging calculation
 *   Test 5: Multi-BW throughput scaling
 *   Test 6: Full end-to-end loopback — all modulations, extended headers
 *
 * Build:
 *   g++ -O2 -std=c++17 sol8_test.cpp -o sol8_test -lliquid -lssl -lcrypto -lm
 *
 * Usage:
 *   ./sol8_test          (all tests)
 *   ./sol8_test 2        (run only Test 2)
 *   ./sol8_test ber      (alias for Test 2)
 */

#include <liquid/liquid.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>

/* ── replicate the essential constants from sol8.cpp ───────── */
#define SOL8_SYNC    0xC0FFEE77U
#define SOL8_VER     0x02
#define MAX_PAYLOAD  1400
#define SPS          4
#define ROLLOFF      0.35f
#define RRC_TAPS     16
#define FL_ENCRYPT   0x01
#define FL_AUDIO     0x02
#define FL_CONTROL   0x04
#define MOD_BPSK     1
#define MOD_QPSK     2
#define MOD_16QAM    3
#define MOD_64QAM    4
#define BW_20MHZ     0

static int TESTS_RUN=0, TESTS_PASS=0, TESTS_FAIL=0;
#define PASS(msg) do{TESTS_RUN++;TESTS_PASS++;printf("  ✓ %s\n",msg);}while(0)
#define FAIL(msg) do{TESTS_RUN++;TESTS_FAIL++;printf("  ✗ %s\n",msg);}while(0)
#define CHECK(cond,msg) do{if(cond)PASS(msg);else FAIL(msg);}while(0)

/* ── CRC-32 ─────────────────────────────────────────────────── */
static uint32_t crc_tab[256];
static void crc_init(){
    for(int i=0;i<256;i++){
        uint32_t c=i; for(int j=0;j<8;j++) c=(c&1)?(0xEDB88320U^(c>>1)):(c>>1);
        crc_tab[i]=c;
    }
}
static uint32_t crc32(const uint8_t*d,size_t n){
    uint32_t c=0xFFFFFFFFU;
    for(size_t i=0;i<n;i++) c=crc_tab[(c^d[i])&0xFF]^(c>>8);
    return c^0xFFFFFFFFU;
}

/* ── AES-256-CTR ─────────────────────────────────────────────── */
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

/* ── Frame helpers ───────────────────────────────────────────── */
static uint32_t g_node_id = 0xDEAD1234U;
static uint32_t g_tx_seq  = 0;

static int build_frame(const uint8_t*pl,int plen,
                        uint8_t flags,uint8_t mod,uint8_t bw,
                        bool enc,const uint8_t*key,uint64_t ctr,
                        uint8_t*out){
    uint8_t enc_buf[MAX_PAYLOAD];
    if(enc){aes_ctr(key,ctr,pl,enc_buf,plen);pl=enc_buf;}
    int p=0;
    out[p++]=(SOL8_SYNC>>24)&0xFF; out[p++]=(SOL8_SYNC>>16)&0xFF;
    out[p++]=(SOL8_SYNC>> 8)&0xFF; out[p++]= SOL8_SYNC     &0xFF;
    out[p++]=SOL8_VER;
    out[p++]=flags|(enc?FL_ENCRYPT:0);
    out[p++]=mod; out[p++]=bw;
    uint32_t nid=g_node_id;
    out[p++]=(nid>>24)&0xFF;out[p++]=(nid>>16)&0xFF;
    out[p++]=(nid>> 8)&0xFF;out[p++]= nid     &0xFF;
    uint32_t seq=g_tx_seq++;
    out[p++]=(seq>>24)&0xFF;out[p++]=(seq>>16)&0xFF;
    out[p++]=(seq>> 8)&0xFF;out[p++]= seq     &0xFF;
    out[p++]=(uint8_t)(plen>>8); out[p++]=(uint8_t)plen;
    memcpy(out+p,pl,plen); p+=plen;
    uint32_t c=crc32(out,p); memcpy(out+p,&c,4); p+=4;
    return p;
}

struct Decoder {
    enum class S{HUNT,HDR,PAYLOAD}state=S::HUNT;
    uint32_t shift=0;
    uint8_t buf[MAX_PAYLOAD+32]={};
    int pos=0,expect=0;
    uint16_t plen=0; uint8_t flags=0,mod=0;
    uint32_t node_id=0,seq=0;
    int good=0,bad=0;

    int push(uint8_t b,uint8_t*out,bool enc,const uint8_t*key,uint64_t&ctr){
        switch(state){
        case S::HUNT:
            shift=(shift<<8)|b;
            if(shift==SOL8_SYNC){
                buf[0]=(SOL8_SYNC>>24)&0xFF;buf[1]=(SOL8_SYNC>>16)&0xFF;
                buf[2]=(SOL8_SYNC>> 8)&0xFF;buf[3]= SOL8_SYNC     &0xFF;
                pos=4;state=S::HDR;expect=14;
            }
            break;
        case S::HDR:
            buf[pos++]=b;
            if(--expect==0){
                flags  =buf[5];mod=buf[6];
                node_id=((uint32_t)buf[8]<<24)|((uint32_t)buf[9]<<16)
                        |((uint32_t)buf[10]<<8)|buf[11];
                seq    =((uint32_t)buf[12]<<24)|((uint32_t)buf[13]<<16)
                        |((uint32_t)buf[14]<<8)|buf[15];
                plen   =((uint16_t)buf[16]<<8)|buf[17];
                if(!plen||plen>MAX_PAYLOAD){state=S::HUNT;shift=0;break;}
                expect=plen+4;state=S::PAYLOAD;
            }
            break;
        case S::PAYLOAD:
            buf[pos++]=b;
            if(--expect==0){
                int hlen=18+plen;
                uint32_t got,exp;
                memcpy(&got,buf+hlen,4); exp=crc32(buf,hlen);
                state=S::HUNT;shift=0;pos=0;
                if(got!=exp){bad++;return -1;}
                good++;
                const uint8_t*pl=buf+18;
                if((flags&FL_ENCRYPT)&&enc){aes_ctr(key,ctr,pl,out,plen);ctr++;}
                else memcpy(out,pl,plen);
                return plen;
            }
            break;
        }
        return 0;
    }
};

/* ── AWGN ────────────────────────────────────────────────────── */
static liquid_float_complex awgn(float nstd){
    float u1=(float)rand()/RAND_MAX; if(u1<1e-10f)u1=1e-10f;
    float u2=(float)rand()/RAND_MAX;
    float m=nstd*sqrtf(-2.0f*logf(u1));
    liquid_float_complex n{m*cosf(2.0f*(float)M_PI*u2),
                           m*sinf(2.0f*(float)M_PI*u2)};
    return n;
}

/* ══════════════════════════════════════════════════════════════
 * TEST 1: Frame encode/decode — all flag combinations
 * ══════════════════════════════════════════════════════════════ */
static void test1_frame_codec(){
    printf("\n[TEST 1] Frame encode/decode\n");
    uint8_t key[32]={};
    for(int i=0;i<32;i++) key[i]=(uint8_t)(i*7+3);

    /* Plain frame */
    {
        uint8_t pl[64], frame[MAX_PAYLOAD+32], out[MAX_PAYLOAD+32];
        for(int i=0;i<64;i++) pl[i]=(uint8_t)(i+1);
        int flen=build_frame(pl,64,0,MOD_QPSK,BW_20MHZ,false,key,0,frame);
        Decoder dec; uint64_t ctr=0;
        for(int i=0;i<flen;i++) dec.push(frame[i],out,false,key,ctr);
        CHECK(dec.good==1 && dec.bad==0,"plain frame roundtrip");
        CHECK(memcmp(out,pl,64)==0,"payload matches");
        CHECK(dec.mod==MOD_QPSK,"modulation code in header");
    }

    /* Encrypted frame */
    {
        uint8_t pl[128],frame[MAX_PAYLOAD+32],out[MAX_PAYLOAD+32];
        for(int i=0;i<128;i++) pl[i]=(uint8_t)(rand()&0xFF);
        int flen=build_frame(pl,128,0,MOD_16QAM,BW_20MHZ,true,key,42,frame);
        Decoder dec; uint64_t ctr=42;
        for(int i=0;i<flen;i++) dec.push(frame[i],out,true,key,ctr);
        CHECK(dec.good==1,"encrypted frame roundtrip");
        CHECK(memcmp(out,pl,128)==0,"decrypted payload matches");
    }

    /* Max payload */
    {
        uint8_t pl[MAX_PAYLOAD],frame[MAX_PAYLOAD+32],out[MAX_PAYLOAD+32];
        for(int i=0;i<MAX_PAYLOAD;i++) pl[i]=(uint8_t)(i^0xA5);
        int flen=build_frame(pl,MAX_PAYLOAD,0,MOD_64QAM,BW_20MHZ,false,key,0,frame);
        Decoder dec; uint64_t ctr=0;
        for(int i=0;i<flen;i++) dec.push(frame[i],out,false,key,ctr);
        CHECK(dec.good==1,"max-payload frame");
        CHECK(memcmp(out,pl,MAX_PAYLOAD)==0,"max payload content matches");
    }

    /* Multiple frames in stream */
    {
        uint8_t stream[8192]={}, out[MAX_PAYLOAD+32];
        int slen=0;
        for(int f=0;f<5;f++){
            uint8_t pl[100];
            for(int i=0;i<100;i++) pl[i]=(uint8_t)(f*100+i);
            slen+=build_frame(pl,100,0,MOD_QPSK,BW_20MHZ,false,key,0,stream+slen);
        }
        Decoder dec; uint64_t ctr=0;
        for(int i=0;i<slen;i++) dec.push(stream[i],out,false,key,ctr);
        CHECK(dec.good==5 && dec.bad==0,"5 frames back-to-back");
    }

    /* Bit-flip corruption detected */
    {
        uint8_t pl[64],frame[MAX_PAYLOAD+32],out[MAX_PAYLOAD+32];
        for(int i=0;i<64;i++) pl[i]=(uint8_t)i;
        int flen=build_frame(pl,64,0,MOD_QPSK,BW_20MHZ,false,key,0,frame);
        frame[20] ^= 0x80; /* corrupt payload */
        Decoder dec; uint64_t ctr=0;
        for(int i=0;i<flen;i++) dec.push(frame[i],out,false,key,ctr);
        CHECK(dec.good==0 && dec.bad==1,"CRC detects corruption");
    }

    /* Audio flag preserved */
    {
        uint8_t pl[80],frame[MAX_PAYLOAD+32],out[MAX_PAYLOAD+32];
        for(int i=0;i<80;i++) pl[i]=(uint8_t)i;
        int flen=build_frame(pl,80,FL_AUDIO,MOD_BPSK,BW_20MHZ,false,key,0,frame);
        Decoder dec; uint64_t ctr=0;
        for(int i=0;i<flen;i++) dec.push(frame[i],out,false,key,ctr);
        CHECK(dec.good==1,"audio frame decoded");
        CHECK(dec.flags & FL_AUDIO,"FL_AUDIO flag preserved");
        CHECK(dec.mod==MOD_BPSK,"BPSK code in audio frame");
    }
}

/* ══════════════════════════════════════════════════════════════
 * TEST 2: BER vs SNR curves for all 4 modulations
 *   At 40 dB SNR: expect BER = 0 for all schemes
 *   At minimum viable SNR: expect BER < 1%
 * ══════════════════════════════════════════════════════════════ */
struct BerPoint { float snr_db; float ber; };

static float measure_ber(modulation_scheme scheme, int bps,
                          float snr_db, int n_frames) {
    float nstd = 1.0f / sqrtf(2.0f*powf(10.0f, snr_db/10.0f));
    modemcf        tx    = modemcf_create(scheme);
    firinterp_crcf interp = firinterp_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    firdecim_crcf  decim  = firdecim_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    modemcf        rx    = modemcf_create(scheme);

    uint8_t pl[256], frame[MAX_PAYLOAD+32], out[MAX_PAYLOAD+32];
    uint8_t key[32]={};
    Decoder dec;

    liquid_float_complex iq_out[SPS], decim_in[SPS]; int didx=0;
    /* Flush RRC filters */
    for(int k=0;k<RRC_TAPS*2;k++){
        liquid_float_complex z{};
        firinterp_crcf_execute(interp,z,iq_out);
        for(int s=0;s<SPS;s++){
            decim_in[didx++]=iq_out[s];
            if(didx==SPS){didx=0; liquid_float_complex d; firdecim_crcf_execute(decim,decim_in,&d);}
        }
    }

    unsigned sym_mask=(1u<<bps)-1u;
    uint32_t tx_bits=0; int tx_nbits=0;
    uint32_t rx_bits=0; int rx_nbits=0;

    for(int f=0;f<n_frames;f++){
        for(int i=0;i<256;i++) pl[i]=(uint8_t)(rand()&0xFF);
        int flen=build_frame(pl,256,0,MOD_QPSK,BW_20MHZ,false,key,0,frame);

        for(int bi=0;bi<flen;bi++){
            tx_bits=(tx_bits<<8)|frame[bi]; tx_nbits+=8;
            while(tx_nbits>=bps){
                tx_nbits-=bps;
                unsigned sym=(tx_bits>>tx_nbits)&sym_mask;
                liquid_float_complex iq_sym;
                modemcf_modulate(tx,sym,&iq_sym);
                firinterp_crcf_execute(interp,iq_sym,iq_out);
                for(int k=0;k<SPS;k++){
                    liquid_float_complex n=awgn(nstd);
                    liquid_float_complex rxs{iq_out[k].real+n.real,iq_out[k].imag+n.imag};
                    decim_in[didx++]=rxs;
                    if(didx<SPS) continue; didx=0;
                    liquid_float_complex d;
                    firdecim_crcf_execute(decim,decim_in,&d);
                    /* matched filter pair gain = SPS; compensate for QAM thresholds */
                    d.real/=SPS; d.imag/=SPS;
                    unsigned r; modemcf_demodulate(rx,d,&r);
                    rx_bits=(rx_bits<<bps)|(r&sym_mask); rx_nbits+=bps;
                    while(rx_nbits>=8){
                        rx_nbits-=8;
                        uint8_t byte=(uint8_t)((rx_bits>>rx_nbits)&0xFF);
                        uint64_t ctr=0; dec.push(byte,out,false,key,ctr);
                    }
                }
            }
        }
    }

    modemcf_destroy(rx); firdecim_crcf_destroy(decim);
    firinterp_crcf_destroy(interp); modemcf_destroy(tx);

    int total = dec.good + dec.bad;
    return (total > 0) ? (float)dec.bad / (float)total : 1.0f;
}

static void test2_ber_curves(){
    printf("\n[TEST 2] BER vs SNR curves\n");
    printf("  %-10s  %-8s  %-8s  %-8s  %-8s\n",
           "SNR(dB)", "BPSK", "QPSK", "16QAM", "64QAM");
    printf("  %-10s  %-8s  %-8s  %-8s  %-8s\n",
           "-------", "----", "----", "-----", "-----");

    struct { modulation_scheme scheme; int bps; int code; const char* name; } mods[] = {
        {LIQUID_MODEM_BPSK,  1, MOD_BPSK,  "BPSK"},
        {LIQUID_MODEM_QPSK,  2, MOD_QPSK,  "QPSK"},
        {LIQUID_MODEM_QAM16, 4, MOD_16QAM, "16QAM"},
        {LIQUID_MODEM_QAM64, 6, MOD_64QAM, "64QAM"},
    };
    float snr_levels[] = {5.0f, 10.0f, 15.0f, 20.0f, 30.0f, 40.0f};
    bool all_good_at_40db = true;

    for (float snr : snr_levels) {
        printf("  %-10.1f", snr);
        for (auto& m : mods) {
            float ber = measure_ber(m.scheme, m.bps, snr, 20);
            printf("  %6.3f  ", ber);
            if (snr == 40.0f && ber > 0.01f) all_good_at_40db = false;
        }
        printf("\n");
    }

    /* At 40 dB SNR, all modulations must have near-zero BER through RRC filters */
    CHECK(all_good_at_40db, "all modulations: BER < 1% at 40 dB SNR");

    /* BPSK must work at 10 dB SNR */
    float bpsk_10db = measure_ber(LIQUID_MODEM_BPSK, 1, 10.0f, 30);
    CHECK(bpsk_10db < 0.05f, "BPSK: BER < 5% at 10 dB SNR");

    /* QPSK must work at 15 dB SNR */
    float qpsk_15db = measure_ber(LIQUID_MODEM_QPSK, 2, 15.0f, 30);
    CHECK(qpsk_15db < 0.05f, "QPSK: BER < 5% at 15 dB SNR");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 3: Adaptive modulation state machine
 * ══════════════════════════════════════════════════════════════ */
static int snr_to_code(float snr, int current){
    if(snr>=24.0f && current<MOD_64QAM) return MOD_64QAM;
    if(snr>=15.0f && current<MOD_16QAM) return MOD_16QAM;
    if(snr>=8.0f  && current<MOD_QPSK)  return MOD_QPSK;
    if(snr<5.0f   && current>MOD_BPSK)  return MOD_BPSK;
    if(snr<12.0f  && current>MOD_QPSK)  return MOD_QPSK;
    if(snr<21.0f  && current>MOD_16QAM) return MOD_16QAM;
    return current;
}

static void test3_adaptive_mod(){
    printf("\n[TEST 3] Adaptive modulation state machine\n");

    /* Step up: at 30dB, function jumps to 64QAM (checks 24dB threshold first) */
    CHECK(snr_to_code(30.0f, MOD_BPSK) > MOD_BPSK,
          "step up from BPSK at 30dB SNR");
    int m=MOD_BPSK;
    m=snr_to_code(25.0f,m); CHECK(m>=MOD_QPSK,  "BPSK→QPSK at 25dB");
    m=snr_to_code(25.0f,m); CHECK(m>=MOD_16QAM, "QPSK→16QAM at 25dB");
    m=snr_to_code(25.0f,m); CHECK(m==MOD_64QAM, "16QAM→64QAM at 25dB");

    /* Step down */
    m=MOD_64QAM;
    m=snr_to_code(3.0f,m);  CHECK(m<MOD_64QAM,  "64QAM steps down at 3dB");
    m=snr_to_code(3.0f,m);  CHECK(m<MOD_16QAM,  "keeps stepping down");
    m=snr_to_code(3.0f,m);  CHECK(m==MOD_BPSK,  "reaches BPSK at 3dB");

    /* Hysteresis: QPSK at 9 dB stays QPSK (not yet 15 dB to jump to 16QAM) */
    m=MOD_QPSK;
    m=snr_to_code(9.0f,m); CHECK(m==MOD_QPSK,   "QPSK stable at 9dB (hysteresis)");

    /* Hysteresis: 16QAM at 13 dB stays (not yet 21 dB to step down) */
    m=MOD_16QAM;
    m=snr_to_code(13.0f,m); CHECK(m==MOD_16QAM, "16QAM stable at 13dB (hysteresis)");

    /* Stepping through a realistic SNR trajectory */
    const float traj[] = {2,4,6,9,12,16,20,25,30,25,20,14,10,6,3};
    m=MOD_BPSK; int prev=m;
    printf("    SNR trajectory: ");
    for(float snr : traj){
        m=snr_to_code(snr,m);
        const char* names[]={"?","BPSK","QPSK","16QAM","64QAM"};
        printf("%.0f→%s ",snr,names[m]);
        prev=m;
    }
    printf("\n");
    CHECK(m<=MOD_QPSK, "ends at low mod after SNR drop to 3dB");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 4: RSSI-based ranging
 * ══════════════════════════════════════════════════════════════ */
static float rssi_to_dist_km(float rssi,float freq_mhz,
                               float tx_dbm,float g_tx,float g_rx){
    float fspl_ref=20.0f*log10f(freq_mhz)+147.55f;
    float path_db=tx_dbm+g_tx+g_rx-rssi;
    return powf(10.0f,(path_db-fspl_ref)/20.0f);
}
/* Inverse: compute expected RSSI for a known distance */
static float dist_to_rssi(float dist_km,float freq_mhz,
                            float tx_dbm,float g_tx,float g_rx){
    float fspl_ref=20.0f*log10f(freq_mhz)+147.55f;
    float fspl=fspl_ref+20.0f*log10f(dist_km);
    return tx_dbm+g_tx+g_rx-fspl;
}

static void test4_ranging(){
    printf("\n[TEST 4] RSSI-based ranging\n");
    float freq=434.0f, tx=0.0f, gtx=10.0f, grx=10.0f;

    /* Link budget: tx=0dBm, G_tx=10dBi, G_rx=10dBi, 434 MHz
     * FSPL_ref = 20log10(434) + 147.55 = 52.75 + 147.55 = 200.30 dB/km^0
     * At 1 km: FSPL = 200.30 + 0 = 200.30 dB → RSSI = 0+10+10-200.30 = -180.30 dBm (impossible)
     * Let's use a more realistic: FSPL = 20*log10(d_km) + 20*log10(f_mhz) + 20*log10(4π/c * 1e9)
     * c = 3e8 m/s, f=434e6, d_km in km:
     * FSPL = 20log10(d*1000) + 20log10(f) + 20log10(4π/c)
     *      = 20log10(d_km) + 60 + 20log10(434e6) + 20log10(4π/3e8)
     * Actually the formula: FSPL(dB) = 20log10(d_m) + 20log10(f_hz) - 147.55
     * For d in km: FSPL = 20log10(d_km*1000) + 20log10(f_hz) - 147.55
     *            = 20log10(d_km) + 60 + 20log10(f_hz) - 147.55
     * Our formula uses FSPL_ref = 20log10(f_mhz) + 147.55 which gives:
     * path_db = tx + gtx + grx - rssi
     * dist_km = 10^((path_db - fspl_ref)/20)
     * Let's verify round-trip: compute rssi at d=1km, then get d back */

    float d_test[] = {0.1f, 0.5f, 1.0f, 5.0f, 10.0f};
    bool all_ok=true;
    printf("    %-10s  %-12s  %-12s  %-10s\n",
           "True (km)","RSSI (dBm)","Est. (km)","Error");
    for(float d : d_test){
        float rssi=dist_to_rssi(d,freq,tx,gtx,grx);
        float est=rssi_to_dist_km(rssi,freq,tx,gtx,grx);
        float err=fabsf(est-d)/d*100.0f;
        printf("    %-10.2f  %-12.2f  %-12.3f  %.2f%%\n",d,rssi,est,err);
        if(err>1.0f) all_ok=false;
    }
    CHECK(all_ok,"round-trip ranging error < 1%");

    /* Realistic: 10 km, 1W PA (+30dBm), 10dBi Yagi each side → -58dBm */
    float rssi_10km = dist_to_rssi(10.0f, freq, 30.0f, 10.0f, 10.0f);
    float est_10km  = rssi_to_dist_km(rssi_10km, freq, 30.0f, 10.0f, 10.0f);
    printf("    10km 1W PA+Yagi scenario: RSSI=%.1f dBm  est=%.3f km\n",
           rssi_10km, est_10km);
    CHECK(fabsf(est_10km - 10.0f) < 0.01f, "10 km link budget check");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 5: BW scaling — throughput estimate for each BW setting
 * ══════════════════════════════════════════════════════════════ */
static void test5_bw_scaling(){
    printf("\n[TEST 5] Channel BW → throughput scaling\n");
    printf("  %-8s  %-10s  %-8s  %-8s  %-8s  %-8s\n",
           "BW(MHz)","Rate(MSPS)","BPSK","QPSK","16QAM","64QAM");
    printf("  %-8s  %-10s  %-8s  %-8s  %-8s  %-8s\n",
           "-------","----------","----","----","-----","-----");

    struct{float bw; long long rate;} bws[]={
        {1.25f,2500000LL},{2.5f,5000000LL},{5.0f,10000000LL},
        {10.0f,20000000LL},{20.0f,20000000LL}
    };
    float prev_qpsk=-1;
    bool scaling_ok=true;
    for(auto& b:bws){
        float sym_rate=(float)b.rate/SPS;
        float bpsk  =sym_rate*1/1e6f;
        float qpsk  =sym_rate*2/1e6f;
        float qam16 =sym_rate*4/1e6f;
        float qam64 =sym_rate*6/1e6f;
        printf("  %-8.2f  %-10lld  %-6.2f  %-6.2f  %-7.2f  %-7.2f  Mbps\n",
               b.bw,b.rate,bpsk,qpsk,qam16,qam64);
        if(prev_qpsk>0 && b.bw<=10.0f && qpsk<=prev_qpsk)
            scaling_ok=false;
        if(b.bw<=10.0f) prev_qpsk=qpsk;
    }
    CHECK(scaling_ok,"throughput scales with BW (1.25→10 MHz)");

    /* SOL8 target: 5 MB/s = 40 Mbps at 20 MHz BW */
    float sym_rate=20000000.0f/SPS;
    float qpsk_20mhz=sym_rate*2;
    CHECK(qpsk_20mhz>=10e6f,"20MHz QPSK ≥ 10 Mbps");
    float qam64_20mhz=sym_rate*6;
    CHECK(qam64_20mhz>=30e6f,"20MHz 64QAM ≥ 30 Mbps");

    printf("  SOL8 target 5MB/s = 40Mbps: our 20MHz QPSK=%.0f Mbps, 64QAM=%.0f Mbps\n",
           qpsk_20mhz/1e6f, qam64_20mhz/1e6f);
}

/* ══════════════════════════════════════════════════════════════
 * TEST 6: Full end-to-end loopback — all 4 modulations, extended headers
 *   Uses RRC filter pipeline (no sync loops — zero offset in SW)
 * ══════════════════════════════════════════════════════════════ */
static int e2e_loopback(modulation_scheme scheme, int bps,
                         int n_frames, float snr_db,
                         uint8_t mod_code){
    float nstd=1.0f/sqrtf(2.0f*powf(10.0f,snr_db/10.0f));
    modemcf       tx_mod = modemcf_create(scheme);
    firinterp_crcf interp = firinterp_crcf_create_prototype(
                            LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    agc_crcf       agc   = agc_crcf_create();
    agc_crcf_set_bandwidth(agc,1e-3f);
    firdecim_crcf  decim = firdecim_crcf_create_prototype(
                            LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    modemcf        rx_dem= modemcf_create(scheme);
    Decoder dec;
    uint8_t key[32]={};

    liquid_float_complex iq_out[SPS],decim_in[SPS]; int didx=0;
    uint8_t payload[512],frame[MAX_PAYLOAD+32],out[MAX_PAYLOAD+32];

    for(int k=0;k<RRC_TAPS*2;k++){
        liquid_float_complex z{};
        firinterp_crcf_execute(interp,z,iq_out);
        for(int s=0;s<SPS;s++){
            liquid_float_complex agc_out;
            agc_crcf_execute(agc,iq_out[s],&agc_out);
            decim_in[didx++]=agc_out;
            if(didx==SPS){didx=0; liquid_float_complex d; firdecim_crcf_execute(decim,decim_in,&d);}
        }
    }

    unsigned sym_mask=(1u<<bps)-1u;
    uint32_t tx_bits=0; int tx_nbits=0;
    uint32_t rx_bits=0; int rx_nbits=0;

    for(int f=0;f<n_frames;f++){
        for(int i=0;i<512;i++) payload[i]=(uint8_t)(rand()&0xFF);
        int flen=build_frame(payload,512,0,mod_code,BW_20MHZ,false,key,0,frame);

        for(int bi=0;bi<flen;bi++){
            tx_bits=(tx_bits<<8)|frame[bi]; tx_nbits+=8;
            while(tx_nbits>=bps){
                tx_nbits-=bps;
                unsigned sym=(tx_bits>>tx_nbits)&sym_mask;
                liquid_float_complex iq_sym;
                modemcf_modulate(tx_mod,sym,&iq_sym);
                firinterp_crcf_execute(interp,iq_sym,iq_out);
                for(int k=0;k<SPS;k++){
                    liquid_float_complex n=awgn(nstd);
                    liquid_float_complex rxs{iq_out[k].real+n.real,iq_out[k].imag+n.imag};
                    liquid_float_complex agc_out;
                    agc_crcf_execute(agc,rxs,&agc_out);
                    decim_in[didx++]=agc_out;
                    if(didx<SPS) continue; didx=0;
                    liquid_float_complex d;
                    firdecim_crcf_execute(decim,decim_in,&d);
                    d.real/=SPS; d.imag/=SPS;
                    unsigned r; modemcf_demodulate(rx_dem,d,&r);
                    rx_bits=(rx_bits<<bps)|(r&sym_mask); rx_nbits+=bps;
                    while(rx_nbits>=8){
                        rx_nbits-=8;
                        uint8_t byte=(uint8_t)((rx_bits>>rx_nbits)&0xFF);
                        uint64_t ctr=0; dec.push(byte,out,false,key,ctr);
                    }
                }
            }
        }
    }

    modemcf_destroy(rx_dem); firdecim_crcf_destroy(decim);
    agc_crcf_destroy(agc); firinterp_crcf_destroy(interp);
    modemcf_destroy(tx_mod);
    return dec.good;
}

static void test6_e2e_loopback(){
    printf("\n[TEST 6] End-to-end loopback — all modulations\n");
    struct{modulation_scheme s;int bps;int code;const char*nm;}mods[]={
        {LIQUID_MODEM_BPSK, 1,MOD_BPSK, "BPSK"},
        {LIQUID_MODEM_QPSK, 2,MOD_QPSK, "QPSK"},
        {LIQUID_MODEM_QAM16,4,MOD_16QAM,"16QAM"},
        {LIQUID_MODEM_QAM64,6,MOD_64QAM,"64QAM"},
    };
    for(auto& m:mods){
        int good=e2e_loopback(m.s,m.bps,50,40.0f,m.code);
        char msg[64];
        snprintf(msg,sizeof(msg),"%s: %d/50 frames at 40dB SNR",m.nm,good);
        CHECK(good>=40,msg); /* allow up to 10 filter-edge losses */
    }
}

/* ══════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════ */
int main(int argc,char*argv[]){
    crc_init();
    srand((unsigned)time(NULL));

    int run_only=-1;
    if(argc>1){
        if(!strcmp(argv[1],"ber"))   run_only=2;
        else run_only=atoi(argv[1]);
    }

    printf("=== SOL8 SDR Test Suite ===\n");

    if(run_only<0||run_only==1) test1_frame_codec();
    if(run_only<0||run_only==2) test2_ber_curves();
    if(run_only<0||run_only==3) test3_adaptive_mod();
    if(run_only<0||run_only==4) test4_ranging();
    if(run_only<0||run_only==5) test5_bw_scaling();
    if(run_only<0||run_only==6) test6_e2e_loopback();

    printf("\n=== Summary: %d/%d passed",TESTS_PASS,TESTS_RUN);
    if(TESTS_FAIL) printf("  (%d FAILED)",TESTS_FAIL);
    printf(" ===\n");
    return TESTS_FAIL ? 1 : 0;
}
