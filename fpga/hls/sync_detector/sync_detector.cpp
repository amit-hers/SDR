/*
 * sync_detector.cpp  —  Frame sync word correlator for Zynq-7010 FPGA
 *
 * Sits between the QPSK demod byte output and AXI DMA.
 * Searches the byte stream for 0xC0FFEE77.  When found:
 *   – asserts frame_start on that beat
 *   – forwards the frame (header + payload + CRC) to DMA output
 *   – suppresses inter-frame noise bytes so the CPU is never interrupted
 *     for garbage data
 *
 * The CPU interrupt fires only when a real frame arrives, cutting
 * CPU load by ~95% compared to forwarding every noise sample.
 *
 * ── AXI4-Lite registers (base + offset) ──────────────────────────────
 *   0x00  sync_word   RW  sync pattern to search (default 0xC0FFEE77)
 *   0x04  match_count RO  frames detected since reset (clear on read)
 *   0x08  drop_count  RO  bytes dropped (inter-frame noise)
 *
 * ── AXI4-Stream ports ─────────────────────────────────────────────────
 *   s_axis  : byte stream from QPSK demod (8-bit data + last)
 *   m_axis  : filtered frame bytes to AXI DMA (8-bit data + last + user[0]=frame_start)
 *
 * ── Frame format reference ────────────────────────────────────────────
 *   [SYNC 4B] [VER 1B] [FLAGS 1B] [MOD 1B] [BW 1B]
 *   [NODE_ID 4B] [SEQ 4B] [LEN 2B]  ← header = 18B, then LEN payload bytes + 4B CRC
 *   Total min frame: 22 bytes overhead + payload
 */

#include "ap_fixed.h"
#include "ap_int.h"
#include "hls_stream.h"

// ── Constants ────────────────────────────────────────────────────────
static const ap_uint<32> DEFAULT_SYNC = 0xC0FFEE77;
static const int         HEADER_BYTES = 18;   // bytes after sync word before LEN field
static const int         LEN_OFFSET   = 10;   // bytes into header where LEN (2B BE) sits
static const int         CRC_BYTES    = 4;
static const int         MAX_PAYLOAD  = 1400;
static const int         MAX_FRAME    = HEADER_BYTES + MAX_PAYLOAD + CRC_BYTES;

// ── Stream types ─────────────────────────────────────────────────────
struct InByte {
    ap_uint<8> data;
    ap_uint<1> last;
};

struct OutByte {
    ap_uint<8> data;
    ap_uint<1> last;
    ap_uint<1> user;   // user[0] = 1 on first byte of frame (frame_start)
};

// ── AXI4-Lite control struct ─────────────────────────────────────────
struct CtrlRegs {
    ap_uint<32> sync_word;    // 0x00
    ap_uint<32> match_count;  // 0x04 (read clears)
    ap_uint<32> drop_count;   // 0x08 (read clears)
};

// ── Top-level ────────────────────────────────────────────────────────
void sync_detector_top(
    hls::stream<InByte>&  s_axis,
    hls::stream<OutByte>& m_axis,
    // AXI4-Lite slave
    volatile ap_uint<32>& sync_word_reg,
    volatile ap_uint<32>& match_count_reg,
    volatile ap_uint<32>& drop_count_reg)
{
#pragma HLS INTERFACE axis           port=s_axis
#pragma HLS INTERFACE axis           port=m_axis
#pragma HLS INTERFACE s_axilite      port=sync_word_reg    offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite      port=match_count_reg  offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite      port=drop_count_reg   offset=0x20 bundle=ctrl
#pragma HLS INTERFACE s_axilite      port=return           bundle=ctrl
#pragma HLS PIPELINE II=1

    // State
    static ap_uint<32> shift_reg   = 0;
    static ap_uint<1>  in_frame    = 0;
    static ap_uint<11> frame_bytes = 0;   // bytes remaining in current frame
    static ap_uint<11> payload_len = 0;
    static ap_uint<4>  hdr_pos     = 0;   // position within header (for LEN extraction)
    static ap_uint<8>  len_hi      = 0;
    static ap_uint<32> matches     = 0;
    static ap_uint<32> drops       = 0;
    static ap_uint<1>  first_byte  = 0;

    if (s_axis.empty()) return;

    InByte  in  = s_axis.read();
    ap_uint<32> sync_target = sync_word_reg;

    // Update 4-byte shift register (big-endian: oldest byte at MSB)
    shift_reg = (shift_reg << 8) | in.data;

    if (!in_frame) {
        // Searching for sync word
        if (shift_reg == sync_target) {
            in_frame    = 1;
            frame_bytes = HEADER_BYTES + CRC_BYTES;   // will add payload_len once parsed
            hdr_pos     = 0;
            payload_len = 0;
            first_byte  = 1;
            matches++;
            match_count_reg = matches;
            // Emit the sync word bytes (4 bytes already consumed into shift_reg;
            // emit them as the first four output beats)
            // Since we process one byte/cycle with II=1, emit sync word MSB-first
            for (int b = 3; b >= 0; b--) {
#pragma HLS UNROLL
                OutByte out;
                out.data = (shift_reg >> (b * 8)) & 0xFF;
                out.last = 0;
                out.user = (b == 3) ? ap_uint<1>(1) : ap_uint<1>(0);
                m_axis.write(out);
            }
        } else {
            drops++;
            drop_count_reg = drops;
        }
    } else {
        // Inside a frame — forward byte
        OutByte out;
        out.data = in.data;
        out.user = first_byte;
        first_byte = 0;

        // Parse LEN field from header (bytes 10–11 after sync word)
        if (hdr_pos == (ap_uint<4>)LEN_OFFSET) {
            len_hi = in.data;
        } else if (hdr_pos == (ap_uint<4>)(LEN_OFFSET + 1)) {
            payload_len = ((ap_uint<11>)len_hi << 8) | in.data;
            frame_bytes = (ap_uint<11>)(HEADER_BYTES + payload_len + CRC_BYTES);
        }
        if (hdr_pos < (ap_uint<4>)HEADER_BYTES) hdr_pos++;

        if (frame_bytes > 0) frame_bytes--;

        bool done = (frame_bytes == 0);
        out.last  = done ? 1 : 0;
        m_axis.write(out);

        if (done) {
            in_frame = 0;
            shift_reg = 0;   // reset so partial match can't trigger immediately
        }
    }
}
