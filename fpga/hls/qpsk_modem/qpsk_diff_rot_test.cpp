// Round-trip through BOTH cores, with each of the four phase rotations applied
// to the IQ in between. Absolute QPSK survives exactly one rotation;
// differential should survive all four. That is the whole point of diff_mode.
#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"
#include <cstdio>
#include <cstdint>
#include <vector>
typedef ap_axiu<32,0,0,0> IQSample;
typedef ap_axiu<8,0,0,0>  BitByte;
void qpsk_mod_top(hls::stream<BitByte>&, hls::stream<IQSample>&,
                  volatile ap_uint<1>&, volatile ap_uint<2>&, volatile ap_uint<1>&);
void qpsk_demod_top(hls::stream<IQSample>&, hls::stream<BitByte>&,
                    volatile ap_uint<1>&, volatile ap_uint<32>&,
                    volatile ap_uint<1>&, volatile ap_uint<1>&);

int main(int argc, char** argv) {
    const int NB = 256;
    std::vector<uint8_t> tx(NB);
    for (int i = 0; i < NB; i++) tx[i] = (uint8_t)((i*37 + 11) & 0xFF);   // varied, not a ramp

    printf("  payload %d bytes; rotation applied to the IQ between the cores\n\n", NB);
    printf("  %-14s %s\n", "mode", "longest exact byte run by rotation  0deg  90deg 180deg 270deg");
    for (int diff = 0; diff <= 1; diff++) {
        printf("  %-14s", diff ? "differential" : "absolute");
        for (int rot = 0; rot < 4; rot++) {
            // --- modulate ---
            hls::stream<BitByte> mb("mb"); hls::stream<IQSample> mi("mi");
            volatile ap_uint<1> men = 1, mdiff = diff; volatile ap_uint<2> bpsk = 0;
            std::vector<IQSample> iq;
            for (int i = 0; i < NB; i++) {
                BitByte b; b.data = tx[i]; b.keep = -1; b.strb = -1; b.last = (i+1==NB);
                mb.write(b);
                qpsk_mod_top(mb, mi, men, bpsk, mdiff);
                while (!mi.empty()) iq.push_back(mi.read());
            }
            // --- rotate ---
            for (auto& s : iq) {
                int16_t I = (int16_t)(uint16_t)s.data.range(15,0);
                int16_t Q = (int16_t)(uint16_t)s.data.range(31,16);
                int16_t ri, rq;
                switch (rot) { case 0: ri=I; rq=Q; break;
                               case 1: ri=-Q; rq=I; break;
                               case 2: ri=-I; rq=-Q; break;
                               default: ri=Q; rq=-I; }
                s.data.range(15,0)  = ap_uint<16>((uint16_t)ri);
                s.data.range(31,16) = ap_uint<16>((uint16_t)rq);
            }
            // --- demodulate ---
            hls::stream<IQSample> di("di"); hls::stream<BitByte> db("db");
            volatile ap_uint<1> den = 1, drst = 1, ddiff = diff; volatile ap_uint<32> lk = 0;
            qpsk_demod_top(di, db, den, lk, drst, ddiff); drst = 0;
            while (!db.empty()) db.read();
            std::vector<uint8_t> rx;
            for (auto s : iq) {
                di.write(s);
                qpsk_demod_top(di, db, den, lk, drst, ddiff);
                while (!db.empty()) rx.push_back((uint8_t)db.read().data);
            }
            // --- longest exact run at any offset ---
            size_t best = 0;
            for (size_t off = 0; off + 1 < rx.size(); off++) {
                size_t run = 0, cur = 0;
                for (size_t k = 0; off + k < rx.size() && k < tx.size(); k++) {
                    if (rx[off+k] == tx[k]) { if (++cur > run) run = cur; } else cur = 0;
                }
                if (run > best) best = run;
            }
            printf("  %6zu", best);
        }
        printf("\n");
    }
    return 0;
}
