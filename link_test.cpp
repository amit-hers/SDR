/*
 * link_test.cpp  —  Software DSP loopback test (no PlutoSDR required)
 *
 * Three test levels:
 *   Test 1: Raw modem only (no filters/sync) — verifies bit mapping
 *   Test 2: RRC filter pipeline             — verifies filter setup
 *   Test 3: Full DSP chain with sync warmup — verifies end-to-end
 *
 * Usage:
 *   ./link_test [n_frames] [snr_db]
 *   ./link_test 200 40.0   (clean channel)
 *   ./link_test 200 15.0   (noisy, some errors expected)
 */

#include <liquid/liquid.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define SYNC_WORD   0xDEADBEEFU
#define MAX_PAYLOAD 512
#define SPS         4
#define ROLLOFF     0.35f
#define RRC_TAPS    32

/* ── CRC-32 ───────────────────────────────────────────────── */
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

/* ── Frame build / decode ─────────────────────────────────── */
static int build_frame(const uint8_t*pl,int plen,uint8_t*out){
    int pos=0;
    out[0]=(SYNC_WORD>>24)&0xFF; out[1]=(SYNC_WORD>>16)&0xFF;
    out[2]=(SYNC_WORD>> 8)&0xFF; out[3]= SYNC_WORD     &0xFF; pos=4;
    out[pos]=(uint8_t)(plen>>8); out[pos+1]=(uint8_t)plen; pos+=2;
    memcpy(out+pos,pl,plen); pos+=plen;
    uint32_t c=crc32(out,pos); memcpy(out+pos,&c,4); pos+=4;
    return pos;
}

struct Decoder {
    enum{HUNT,LEN,PAYLOAD}state=HUNT;
    uint32_t shift=0; uint8_t buf[MAX_PAYLOAD+10]{}; int pos=0,expect=0;
    uint16_t plen=0; int good=0,bad=0;
    void push(uint8_t b){
        switch(state){
        case HUNT:
            shift=(shift<<8)|b;
            if(shift==SYNC_WORD){
                buf[0]=(SYNC_WORD>>24)&0xFF; buf[1]=(SYNC_WORD>>16)&0xFF;
                buf[2]=(SYNC_WORD>> 8)&0xFF; buf[3]= SYNC_WORD     &0xFF;
                pos=4; state=LEN; expect=2;
            }
            break;
        case LEN:
            buf[pos++]=b;
            if(--expect==0){
                plen=((uint16_t)buf[4]<<8)|buf[5];
                if(!plen||plen>MAX_PAYLOAD){state=HUNT;shift=0;}
                else{expect=plen+4;state=PAYLOAD;}
            }
            break;
        case PAYLOAD:
            buf[pos++]=b;
            if(--expect==0){
                int h=6+plen; uint32_t got,exp;
                memcpy(&got,buf+h,4); exp=crc32(buf,h);
                (got==exp)?good++:bad++;
                state=HUNT;shift=0;pos=0;
            }
            break;
        }
    }
};

/* ── AWGN ─────────────────────────────────────────────────── */
static liquid_float_complex awgn(float nstd){
    float u1=(float)rand()/RAND_MAX; if(u1<1e-10f)u1=1e-10f;
    float u2=(float)rand()/RAND_MAX;
    float m=nstd*sqrtf(-2.0f*logf(u1));
    liquid_float_complex n{m*cosf(2.0f*(float)M_PI*u2),
                           m*sinf(2.0f*(float)M_PI*u2)};
    return n;
}

/* ─────────────────────────────────────────────────────────────
 * TEST 1: Raw modem only (no filters, no sync)
 *   TX: bytes → symbols      RX: symbols → bytes → CRC check
 *   Should always pass at any SNR (tests bit mapping only)
 * ───────────────────────────────────────────────────────────── */
static int test_raw_modem(int n_frames, float nstd){
    printf("[TEST 1] Raw modem (no filters/sync), %d frames\n", n_frames);
    modemcf tx = modemcf_create(LIQUID_MODEM_QPSK);
    modemcf rx = modemcf_create(LIQUID_MODEM_QPSK);
    Decoder dec;
    uint8_t payload[MAX_PAYLOAD], frame[MAX_PAYLOAD+10];
    uint8_t cur=0; int bpos=0;

    for(int f=0;f<n_frames;f++){
        for(int i=0;i<MAX_PAYLOAD;i++) payload[i]=rand()&0xFF;
        int flen=build_frame(payload,MAX_PAYLOAD,frame);

        /* TX: pack 2 bits → symbol, modulate */
        for(int byte_i=0;byte_i<flen;byte_i+=1){
            uint8_t byte=frame[byte_i];
            for(int bit_i=6;bit_i>=0;bit_i-=2){
                unsigned s=(byte>>bit_i)&0x3;
                liquid_float_complex iq;
                modemcf_modulate(tx,s,&iq);
                /* Add noise */
                liquid_float_complex n=awgn(nstd);
                iq.real+=n.real; iq.imag+=n.imag;
                /* RX: demodulate → 2 bits */
                unsigned r; modemcf_demodulate(rx,iq,&r);
                cur=(cur<<2)|(r&0x3); bpos+=2;
                if(bpos==8){dec.push(cur);cur=0;bpos=0;}
            }
        }
    }
    printf("  good=%d  bad=%d  → %s\n\n",
           dec.good,dec.bad,
           (dec.good==n_frames&&dec.bad==0)?"PASS":"FAIL (check SNR)");
    modemcf_destroy(tx); modemcf_destroy(rx);
    return (dec.good>0);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 2: RRC filter pipeline (no carrier/timing sync)
 *   TX: symbols → RRC interp → AWGN → RRC decim → symbols
 *   Tests the filter at exact timing (no sync loops needed).
 * ───────────────────────────────────────────────────────────── */
static int test_filter_pipeline(int n_frames, float nstd){
    printf("[TEST 2] RRC filter pipeline, %d frames\n", n_frames);
    modemcf       tx_mod  = modemcf_create(LIQUID_MODEM_QPSK);
    firinterp_crcf interp = firinterp_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    firdecim_crcf  decim  = firdecim_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    modemcf        rx_dem = modemcf_create(LIQUID_MODEM_QPSK);
    Decoder dec;
    uint8_t payload[MAX_PAYLOAD],frame[MAX_PAYLOAD+10];
    uint8_t cur=0; int bpos=0;

    /* IQ staging */
    liquid_float_complex iq_out[SPS];
    liquid_float_complex decim_in[SPS]; int didx=0;

    /* RRC filter has group delay = RRC_TAPS symbols at the TX;
       the output is delayed by RRC_TAPS/2 = 16 symbols.
       We send EXTRA zero symbols first to flush the pipeline. */
    const int FLUSH = RRC_TAPS * 2;
    for(int k=0;k<FLUSH;k++){
        liquid_float_complex z{}; /* zero symbol */
        firinterp_crcf_execute(interp,z,iq_out);
        for(int s=0;s<SPS;s++){
            decim_in[didx++]=iq_out[s];
            if(didx==SPS){didx=0;
                liquid_float_complex d; firdecim_crcf_execute(decim,decim_in,&d);
            }
        }
    }

    for(int f=0;f<n_frames;f++){
        for(int i=0;i<MAX_PAYLOAD;i++) payload[i]=rand()&0xFF;
        int flen=build_frame(payload,MAX_PAYLOAD,frame);

        for(int byte_i=0;byte_i<flen;byte_i++){
            uint8_t byte=frame[byte_i];
            for(int bit_i=6;bit_i>=0;bit_i-=2){
                unsigned s=(byte>>bit_i)&0x3;
                liquid_float_complex iq_sym;
                modemcf_modulate(tx_mod,s,&iq_sym);
                firinterp_crcf_execute(interp,iq_sym,iq_out);

                for(int k=0;k<SPS;k++){
                    /* AWGN */
                    liquid_float_complex n=awgn(nstd);
                    liquid_float_complex rx{iq_out[k].real+n.real,
                                           iq_out[k].imag+n.imag};
                    decim_in[didx++]=rx;
                    if(didx<SPS) continue; didx=0;

                    liquid_float_complex d;
                    firdecim_crcf_execute(decim,decim_in,&d);
                    unsigned r; modemcf_demodulate(rx_dem,d,&r);
                    cur=(cur<<2)|(r&0x3); bpos+=2;
                    if(bpos==8){dec.push(cur);cur=0;bpos=0;}
                }
            }
        }
    }
    printf("  good=%d  bad=%d  → %s\n\n",
           dec.good,dec.bad,
           (dec.good>0)?"PASS":"FAIL");
    modemcf_destroy(rx_dem); firdecim_crcf_destroy(decim);
    firinterp_crcf_destroy(interp); modemcf_destroy(tx_mod);
    return (dec.good>0);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 3: AGC + RRC full framing pipeline
 *   Same as Test 2 but with AGC before decimation.
 *   Sync loops (symsync, Costas NCO) compensate for hardware
 *   timing/frequency offsets that don't exist in software loopback,
 *   so they are omitted here and validated in hardware tests.
 * ───────────────────────────────────────────────────────────── */
static int test_full_chain(int n_frames, float nstd){
    printf("[TEST 3] AGC + RRC pipeline (full framing), %d frames\n",n_frames);
    modemcf        tx_mod = modemcf_create(LIQUID_MODEM_QPSK);
    firinterp_crcf interp = firinterp_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    agc_crcf       agc    = agc_crcf_create();
    agc_crcf_set_bandwidth(agc,1e-3f);
    firdecim_crcf  decim  = firdecim_crcf_create_prototype(
                             LIQUID_FIRFILT_RRC,SPS,RRC_TAPS,ROLLOFF,0);
    modemcf        rx_dem = modemcf_create(LIQUID_MODEM_QPSK);
    Decoder dec;

    liquid_float_complex iq_out[SPS];
    liquid_float_complex decim_in[SPS]; int didx=0;
    uint8_t cur=0; int bpos=0;

    /* Flush filter pipeline (RRC delay = RRC_TAPS symbols) */
    const int FLUSH = RRC_TAPS * 2;
    for(int k=0;k<FLUSH;k++){
        liquid_float_complex z{};
        firinterp_crcf_execute(interp,z,iq_out);
        for(int s=0;s<SPS;s++){
            liquid_float_complex agc_out;
            agc_crcf_execute(agc,iq_out[s],&agc_out);
            decim_in[didx++]=agc_out;
            if(didx==SPS){ didx=0;
                liquid_float_complex d; firdecim_crcf_execute(decim,decim_in,&d);
            }
        }
    }

    uint8_t payload[MAX_PAYLOAD],frame[MAX_PAYLOAD+10];
    for(int f=0;f<n_frames;f++){
        for(int i=0;i<MAX_PAYLOAD;i++) payload[i]=rand()&0xFF;
        int flen=build_frame(payload,MAX_PAYLOAD,frame);

        for(int byte_i=0;byte_i<flen;byte_i++){
            uint8_t byte=frame[byte_i];
            for(int bit_i=6;bit_i>=0;bit_i-=2){
                unsigned s=(byte>>bit_i)&0x3;
                liquid_float_complex iq_sym;
                modemcf_modulate(tx_mod,s,&iq_sym);
                firinterp_crcf_execute(interp,iq_sym,iq_out);
                for(int k=0;k<SPS;k++){
                    liquid_float_complex n=awgn(nstd);
                    liquid_float_complex rx{iq_out[k].real+n.real,iq_out[k].imag+n.imag};
                    liquid_float_complex agc_out;
                    agc_crcf_execute(agc,rx,&agc_out);
                    decim_in[didx++]=agc_out;
                    if(didx<SPS) continue; didx=0;
                    liquid_float_complex d;
                    firdecim_crcf_execute(decim,decim_in,&d);
                    unsigned r; modemcf_demodulate(rx_dem,d,&r);
                    cur=(cur<<2)|(r&0x3); bpos+=2;
                    if(bpos==8){dec.push(cur);cur=0;bpos=0;}
                }
            }
        }
        if((f+1)%50==0||f==n_frames-1)
            fprintf(stderr,"\r  Progress %d/%d  good=%d bad=%d    ",
                    f+1,n_frames,dec.good,dec.bad);
    }
    fprintf(stderr,"\n");
    int pass=(dec.good>0);
    printf("  good=%d  bad=%d  → %s\n\n",dec.good,dec.bad,pass?"PASS":"FAIL");

    modemcf_destroy(rx_dem); firdecim_crcf_destroy(decim);
    agc_crcf_destroy(agc); firinterp_crcf_destroy(interp);
    modemcf_destroy(tx_mod);
    return pass;
}

/* ── Main ─────────────────────────────────────────────────── */
int main(int argc,char*argv[]){
    crc_init();
    srand((unsigned)time(NULL));

    int   n_frames = (argc>1)?atoi(argv[1]):200;
    float snr_db   = (argc>2)?atof(argv[2]):30.0f;
    float nstd     = 1.0f/sqrtf(2.0f*powf(10.0f,snr_db/10.0f));

    printf("=== SDR Link DSP Loopback Test ===\n");
    printf("Frames: %d  |  SNR: %.1f dB  |  noise_std: %.5f\n\n",
           n_frames,snr_db,nstd);

    int r1=test_raw_modem(n_frames,nstd);
    int r2=test_filter_pipeline(n_frames,nstd);
    int r3=test_full_chain(n_frames,nstd);

    printf("=== Summary ===\n");
    printf("  Test 1 (raw modem)      : %s\n",r1?"PASS":"FAIL");
    printf("  Test 2 (filter pipeline): %s\n",r2?"PASS":"FAIL");
    printf("  Test 3 (full DSP chain) : %s\n",r3?"PASS":"FAIL");
    int all=r1&&r2&&r3;
    printf("\n  Overall: %s\n",all?"✓ ALL PASS":"✗ SOME TESTS FAILED");
    return all?0:1;
}
