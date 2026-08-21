// Split-modulation protocol tests.
//
// These cover the bootstrap contract: acquisition is always BPSK, the payload
// scheme is whatever the header declares, and the receiver must honour the
// header rather than any local modulation state of its own.
#include "sdr/modem/SplitModem.hpp"
#include "sdr/modem/Modem.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace sdr;

static std::vector<uint8_t> makePayload(size_t n) {
    std::vector<uint8_t> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    return p;
}

// Full loop: frame -> split-modulate -> split-demodulate -> Deframer.
static DecodedFrame roundTrip(const std::vector<uint8_t>& payload,
                              ModCode mod, uint8_t flags = 0,
                              const ReedSolomon* fec = nullptr) {
    Framer framer;
    auto frame = framer.encode(payload, flags, mod, BwCode::BW_1P25,
                               0x00000001, 4242, fec);

    std::vector<std::complex<float>> syms;
    SplitModem::modulate(frame, mod, syms);

    auto r = SplitModem::demodulate(syms, fec != nullptr);
    assert(r.found);
    assert(r.header_ok);
    assert(r.complete);
    assert(r.payload_mod == mod);

    Deframer df;
    std::optional<DecodedFrame> out;
    for (uint8_t b : r.bytes)
        if (auto d = df.push(b, fec, nullptr)) { out = std::move(d); break; }
    assert(out.has_value());
    return *out;
}

void run_splitmod() {
    std::printf("[splitmod] symbol-boundary arithmetic\n");
    {
        // The acquisition section must be an exact whole number of symbols,
        // otherwise the payload boundary drifts by a fraction of a symbol.
        assert(BOOTSTRAP_BYTES == PREAMBLE_LEN + HEADER_SIZE);
        assert(BOOTSTRAP_SYMS  == BOOTSTRAP_BYTES * 8);
        assert(HEADER_SYMS     == HEADER_SIZE * 8);
        assert(modCodeBits(ModCode::BPSK)  == 1);
        assert(modCodeBits(ModCode::QPSK)  == 2);
        assert(modCodeBits(ModCode::QAM16) == 4);
        assert(modCodeBits(ModCode::QAM64) == 6);
        assert(modCodeBits(ModCode::AUTO)  == 0);
        assert(!isWireMod(ModCode::AUTO));

        // Rounding is up: a partial final symbol still has to be transmitted.
        assert(symsForBytes(10, ModCode::BPSK)  == 80);
        assert(symsForBytes(10, ModCode::QPSK)  == 40);
        assert(symsForBytes(10, ModCode::QAM16) == 20);
        assert(symsForBytes(3,  ModCode::QAM64) == 4);   // 24 bits / 6 = 4 exactly
        assert(symsForBytes(1,  ModCode::QAM64) == 2);   // 8 bits / 6 -> 2
    }

    std::printf("[splitmod] BPSK payload decodes\n");
    {
        auto payload = makePayload(242);
        auto d = roundTrip(payload, ModCode::BPSK);
        assert(d.payload == payload);
        assert(d.mod == ModCode::BPSK);
        assert(d.seq == 4242);
    }

    std::printf("[splitmod] QPSK payload on a BPSK bootstrap decodes\n");
    {
        auto payload = makePayload(242);
        auto d = roundTrip(payload, ModCode::QPSK);
        assert(d.payload == payload);
        assert(d.mod == ModCode::QPSK);
    }

    std::printf("[splitmod] payload section really is a different waveform\n");
    {
        // A QPSK-payload burst must be shorter than an all-BPSK one, and its
        // acquisition section must be bit-identical to the BPSK case.
        auto payload = makePayload(242);
        Framer framer;
        auto fb = framer.encode(payload, 0, ModCode::BPSK, BwCode::BW_1P25, 1, 7);
        auto fq = framer.encode(payload, 0, ModCode::QPSK, BwCode::BW_1P25, 1, 7);

        std::vector<std::complex<float>> sb, sq;
        SplitModem::modulate(fb, ModCode::BPSK, sb);
        SplitModem::modulate(fq, ModCode::QPSK, sq);

        // Payload+CRC+postamble at 2 bits/symbol instead of 1.
        const size_t tail = fb.size() - BOOTSTRAP_BYTES;
        assert(sb.size() > sq.size());
        assert(sb.size() == BOOTSTRAP_SYMS + symsForBytes(tail, ModCode::BPSK));
        assert(sq.size() == BOOTSTRAP_SYMS + symsForBytes(tail, ModCode::QPSK));

        // Acquisition sections differ only in the header's mod byte, so the
        // preamble (first PREAMBLE_LEN*8 symbols) must match exactly.
        for (size_t i = 0; i < PREAMBLE_LEN * 8; ++i)
            assert(sb[i] == sq[i]);
    }

    std::printf("[splitmod] receiver follows the header, not its own state\n");
    {
        // The point of the whole refactor: a receiver whose local default is
        // BPSK must still demodulate a QPSK payload correctly, purely because
        // the header said QPSK. Previously the RX used its own adaptive state
        // and this could not work.
        auto payload = makePayload(200);
        Framer framer;
        auto frame = framer.encode(payload, 0, ModCode::QPSK, BwCode::BW_1P25, 1, 99);

        std::vector<std::complex<float>> syms;
        SplitModem::modulate(frame, ModCode::QPSK, syms);

        // No local modulation is passed in anywhere -- demodulate() has only
        // the symbols and the FEC setting to work from.
        auto r = SplitModem::demodulate(syms, false);
        assert(r.header_ok);
        assert(r.payload_mod == ModCode::QPSK);   // came from the wire

        Deframer df;
        std::optional<DecodedFrame> out;
        for (uint8_t b : r.bytes)
            if (auto d = df.push(b, nullptr, nullptr)) { out = std::move(d); break; }
        assert(out.has_value());
        assert(out->payload == payload);
        assert(out->mod == ModCode::QPSK);
    }

    std::printf("[splitmod] 180-degree carrier error recovers for both schemes\n");
    {
        for (ModCode m : {ModCode::BPSK, ModCode::QPSK}) {
            auto payload = makePayload(120);
            Framer framer;
            auto frame = framer.encode(payload, 0, m, BwCode::BW_1P25, 1, 55);
            std::vector<std::complex<float>> syms;
            SplitModem::modulate(frame, m, syms);
            for (auto& s : syms) s = -s;          // whole burst rotated 180°

            auto r = SplitModem::demodulate(syms, false);
            assert(r.found);
            assert(r.inverted);
            assert(r.header_ok);
            assert(r.payload_mod == m);

            Deframer df;
            std::optional<DecodedFrame> out;
            for (uint8_t b : r.bytes)
                if (auto d = df.push(b, nullptr, nullptr)) { out = std::move(d); break; }
            assert(out.has_value());
            assert(out->payload == payload);
        }
    }

    std::printf("[splitmod] offset burst: sync found away from symbol 0\n");
    {
        auto payload = makePayload(64);
        Framer framer;
        auto frame = framer.encode(payload, 0, ModCode::QPSK, BwCode::BW_1P25, 1, 11);
        std::vector<std::complex<float>> syms;
        SplitModem::modulate(frame, ModCode::QPSK, syms);

        // Prepend junk, as a real burst window has before the preamble.
        std::vector<std::complex<float>> pad(137, std::complex<float>(0.2f, -0.1f));
        pad.insert(pad.end(), syms.begin(), syms.end());

        auto r = SplitModem::demodulate(pad, false);
        assert(r.found);
        assert(r.complete);
        // Sync sits PREAMBLE_LEN*8 symbols after the burst start.
        assert(r.sync_sym == 137 + PREAMBLE_LEN * 8);
        assert(r.end_sym <= pad.size());

        Deframer df;
        std::optional<DecodedFrame> out;
        for (uint8_t b : r.bytes)
            if (auto d = df.push(b, nullptr, nullptr)) { out = std::move(d); break; }
        assert(out.has_value());
        assert(out->payload == payload);
    }

    std::printf("[splitmod] sync-word search bound is measured from the burst start\n");
    {
        // The sync word does not sit at the burst start -- it sits one whole
        // preamble later. A caller that passes a bound sized for the
        // *carrier-estimate* search (tens of symbols) instead of the sync
        // search will never find a frame, which looks exactly like a dead
        // link. Pin the distinction down.
        auto payload = makePayload(64);
        Framer framer;
        auto frame = framer.encode(payload, 0, ModCode::BPSK, BwCode::BW_1P25, 1, 8);
        std::vector<std::complex<float>> syms;
        SplitModem::modulate(frame, ModCode::BPSK, syms);

        assert(!SplitModem::demodulate(syms, false, 64).found);
        assert(SplitModem::demodulate(syms, false, PREAMBLE_LEN * 8).found);
    }

    std::printf("[splitmod] FEC-encoded payload sizes the payload section right\n");
    {
        ReedSolomon fec;
        auto payload = makePayload(180);
        auto d = roundTrip(payload, ModCode::QPSK, FL_FEC, &fec);
        assert(d.payload == payload);
    }

    std::printf("[splitmod] truncated burst reports incomplete, not garbage\n");
    {
        auto payload = makePayload(242);
        Framer framer;
        auto frame = framer.encode(payload, 0, ModCode::BPSK, BwCode::BW_1P25, 1, 3);
        std::vector<std::complex<float>> syms;
        SplitModem::modulate(frame, ModCode::BPSK, syms);
        syms.resize(BOOTSTRAP_SYMS + 100);        // header present, payload cut off

        auto r = SplitModem::demodulate(syms, false);
        assert(r.found);
        assert(r.header_ok);
        assert(!r.complete);
        assert(r.bytes.empty());
    }

    std::printf("[splitmod] AUTO is rejected at the modulator\n");
    {
        auto payload = makePayload(16);
        Framer framer;
        auto frame = framer.encode(payload, 0, ModCode::AUTO, BwCode::BW_1P25, 1, 1);
        std::vector<std::complex<float>> syms;
        bool threw = false;
        try { SplitModem::modulate(frame, ModCode::AUTO, syms); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }

    std::printf("[splitmod] header declaring AUTO is refused on receive\n");
    {
        // A corrupted or hostile mod byte must not be fed to the modem.
        auto payload = makePayload(32);
        Framer framer;
        auto frame = framer.encode(payload, 0, ModCode::BPSK, BwCode::BW_1P25, 1, 2);
        frame[PREAMBLE_LEN + 6] = static_cast<uint8_t>(ModCode::AUTO);   // mod field
        std::vector<std::complex<float>> syms;
        SplitModem::modulate(frame, ModCode::BPSK, syms);

        auto r = SplitModem::demodulate(syms, false);
        assert(r.found);
        assert(!r.header_ok);       // rejected before any payload demod
        assert(!r.complete);
    }

    std::printf("[splitmod] OK\n");
}
