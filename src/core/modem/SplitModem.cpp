#include "sdr/modem/SplitModem.hpp"
#include "sdr/modem/Modem.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include <stdexcept>
#include <string>

namespace sdr {

static ModScheme toScheme(ModCode m) {
    switch (m) {
        case ModCode::BPSK:  return ModScheme::BPSK;
        case ModCode::QPSK:  return ModScheme::QPSK;
        case ModCode::QAM16: return ModScheme::QAM16;
        case ModCode::QAM64: return ModScheme::QAM64;
        default: throw std::invalid_argument("SplitModem: not a wire modulation");
    }
}

// ── TX ────────────────────────────────────────────────────────────────────
void SplitModem::modulate(const std::vector<uint8_t>& frame,
                          ModCode payload_mod,
                          std::vector<std::complex<float>>& syms) {
    if (!isWireMod(payload_mod))
        throw std::invalid_argument(
            std::string("SplitModem::modulate: payload modulation '")
            + modCodeName(payload_mod) + "' cannot be transmitted");
    if (frame.size() < BOOTSTRAP_BYTES)
        throw std::invalid_argument("SplitModem::modulate: frame shorter than bootstrap");

    // Acquisition section: always BPSK, always exactly BOOTSTRAP_SYMS symbols.
    Modem boot(ModScheme::BPSK);
    std::vector<std::complex<float>> a;
    boot.modulate(frame.data(), static_cast<int>(BOOTSTRAP_BYTES), a);

    // Payload section: modulated as its own byte stream, so its bit numbering
    // restarts at zero. That is what lets the receiver resume cleanly at the
    // boundary without tracking a fractional bit carry across the two
    // sections.
    Modem pay(toScheme(payload_mod));
    std::vector<std::complex<float>> b;
    pay.modulate(frame.data() + BOOTSTRAP_BYTES,
                 static_cast<int>(frame.size() - BOOTSTRAP_BYTES), b);

    syms.clear();
    syms.reserve(a.size() + b.size());
    syms.insert(syms.end(), a.begin(), a.end());
    syms.insert(syms.end(), b.begin(), b.end());
}

// ── RX ────────────────────────────────────────────────────────────────────
void SplitModem::bpskBits(const std::complex<float>* syms, size_t n,
                          std::vector<uint8_t>& bits) {
    // Go through the same Modem the transmitter used rather than deciding on
    // the sign of the real part directly. The two must agree bit-for-bit: if
    // this side's convention were inverted, every burst would look like it
    // arrived on the opposite carrier phase, the payload would then be
    // negated to "correct" an error that was never there, and the CRC could
    // never pass.
    Modem bpsk(ModScheme::BPSK);
    std::vector<uint8_t> bytes;
    bpsk.demodulate(syms, static_cast<int>(n), bytes);

    bits.resize(n);
    for (size_t i = 0; i < n; ++i)
        bits[i] = (bytes[i / 8] >> (7 - (i % 8))) & 1u;   // MSB-first, as Modem packs
}

SplitModem::Result SplitModem::demodulate(const std::vector<std::complex<float>>& syms,
                                          bool   fec_enabled,
                                          size_t max_search_syms) {
    Result r;
    if (syms.size() < HEADER_SYMS) return r;

    // Only the acquisition section needs BPSK bits, and the sync word must
    // start within the search bound, so demodulate just that much.
    const size_t scan = std::min(syms.size(), max_search_syms + HEADER_SYMS);
    std::vector<uint8_t> bits;
    bpskBits(syms.data(), scan, bits);

    // Hunt the sync word at bit granularity (1 BPSK symbol == 1 bit, so the
    // bit index *is* the symbol index -- this is why the payload boundary
    // below is exact). Accept the complemented word: BPSK carries a 180°
    // ambiguity, and the same 180° error is undone on the payload symbols by
    // negating them.
    uint32_t sr = 0;
    bool hit = false;
    for (size_t i = 0; i < bits.size(); ++i) {
        sr = (sr << 1) | bits[i];
        if (i < 31) continue;
        if (sr == FRAME_SYNC)  { hit = true; r.inverted = false; r.sync_sym = i - 31; break; }
        if (sr == ~FRAME_SYNC) { hit = true; r.inverted = true;  r.sync_sym = i - 31; break; }
    }
    if (!hit) return r;
    r.found = true;

    if (r.sync_sym + HEADER_SYMS > bits.size()) return r;   // header truncated

    // Header: HEADER_SIZE bytes of BPSK, MSB first, matching Modem's packing.
    std::vector<uint8_t> hdr(HEADER_SIZE);
    for (size_t i = 0; i < HEADER_SIZE; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 8; ++k)
            v = (v << 1) | bits[r.sync_sym + i * 8 + static_cast<size_t>(k)];
        if (r.inverted) v = (~v) & 0xFFu;
        hdr[i] = static_cast<uint8_t>(v);
    }
    // Sync bytes are validated by the hunt above; store them canonically so a
    // Deframer fed from `bytes` re-acquires on the un-inverted word.
    hdr[0] = 0xC0; hdr[1] = 0xFF; hdr[2] = 0xEE; hdr[3] = 0x77;

    r.flags       = hdr[5];
    r.payload_mod = static_cast<ModCode>(hdr[6]);
    r.plen        = static_cast<uint16_t>((static_cast<uint16_t>(hdr[16]) << 8) | hdr[17]);

    if (hdr[4] != FRAME_VER)            return r;
    if (r.plen > MAX_PAYLOAD)           return r;
    if (!isWireMod(r.payload_mod))      return r;   // AUTO or garbage on the wire
    r.header_ok = true;

    // Payload length on the wire depends on whether RS was applied.
    size_t wire_payload = r.plen;
    if (fec_enabled && (r.flags & FL_FEC))
        wire_payload = ReedSolomon::encodedSize(r.plen);
    const size_t pay_bytes = wire_payload + CRC_SIZE;

    const size_t pay_start = r.sync_sym + HEADER_SYMS;
    const size_t pay_syms  = symsForBytes(pay_bytes, r.payload_mod);
    r.end_sym = pay_start + pay_syms;
    if (r.end_sym > syms.size()) return r;          // payload truncated
    r.complete = true;

    // Undo a 180° carrier error before handing the payload to its modem. For
    // BPSK this is the classic bit inversion; for QPSK/QAM it is a genuine
    // constellation rotation that no bitwise complement would fix.
    std::vector<std::complex<float>> pay_syms_buf(syms.begin() + static_cast<long>(pay_start),
                                                  syms.begin() + static_cast<long>(r.end_sym));
    if (r.inverted)
        for (auto& s : pay_syms_buf) s = -s;

    Modem pay(toScheme(r.payload_mod));
    std::vector<uint8_t> pay_bytes_buf;
    pay.demodulate(pay_syms_buf.data(), static_cast<int>(pay_syms_buf.size()), pay_bytes_buf);
    // The final symbol may carry padding bits when bits/symbol does not divide
    // the byte count; those land past the CRC and are simply dropped.
    pay_bytes_buf.resize(pay_bytes);

    r.bytes = std::move(hdr);
    r.bytes.insert(r.bytes.end(), pay_bytes_buf.begin(), pay_bytes_buf.end());
    return r;
}

} // namespace sdr
