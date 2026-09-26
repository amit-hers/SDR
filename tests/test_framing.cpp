#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/framing/Frame.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>

static void test_roundtrip() {
    const char* msg = "Hello, RF world!";
    std::vector<uint8_t> payload(reinterpret_cast<const uint8_t*>(msg),
                                  reinterpret_cast<const uint8_t*>(msg) + std::strlen(msg));

    sdr::Framer framer;
    auto encoded = framer.encode(payload, 0x00, sdr::ModCode::QPSK, sdr::BwCode::BW_10,
                                  0xDEADBEEF, 42);
    assert(!encoded.empty());

    sdr::Deframer deframer;
    std::optional<sdr::DecodedFrame> result;
    for (uint8_t b : encoded) {
        result = deframer.push(b, nullptr, nullptr);
        if (result) break;
    }
    assert(result.has_value());
    assert(result->payload == payload);
    assert(result->node_id == 0xDEADBEEF);
    assert(result->seq == 42);
    std::cout << "  [framing] roundtrip: PASS\n";
}

static void test_corrupt_crc() {
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    sdr::Framer framer;
    auto encoded = framer.encode(payload, 0, sdr::ModCode::BPSK, sdr::BwCode::BW_10, 1, 0);

    // Flip a byte in the payload area
    encoded[encoded.size() - 6] ^= 0xFF;

    sdr::Deframer deframer;
    std::optional<sdr::DecodedFrame> result;
    for (uint8_t b : encoded)
        result = deframer.push(b, nullptr, nullptr);

    assert(!result.has_value());
    std::cout << "  [framing] corrupt CRC rejected: PASS\n";
}

static void test_large_payload() {
    std::vector<uint8_t> payload(sdr::MAX_PAYLOAD, 0xAB);
    sdr::Framer framer;
    auto encoded = framer.encode(payload, 0, sdr::ModCode::QAM16, sdr::BwCode::BW_10, 7, 99);

    sdr::Deframer deframer;
    std::optional<sdr::DecodedFrame> result;
    for (uint8_t b : encoded) {
        result = deframer.push(b, nullptr, nullptr);
        if (result) break;
    }
    assert(result.has_value());
    assert(result->payload == payload);
    std::cout << "  [framing] max payload roundtrip: PASS\n";
}

static void test_multi_frame() {
    sdr::Framer framer;
    sdr::Deframer deframer;

    std::vector<uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> pl = {static_cast<uint8_t>(i), static_cast<uint8_t>(i+1)};
        auto f = framer.encode(pl, 0, sdr::ModCode::BPSK, sdr::BwCode::BW_10, 1,
                               static_cast<uint32_t>(i));
        stream.insert(stream.end(), f.begin(), f.end());
    }

    int decoded = 0;
    for (uint8_t b : stream) {
        auto res = deframer.push(b, nullptr, nullptr);
        if (res) ++decoded;
    }
    assert(decoded == 3);
    std::cout << "  [framing] multi-frame stream: PASS\n";
}

// Reed-Solomon must run BEFORE the CRC verdict, not after it.
//
// The deframer used to reject on CRC mismatch and only then reach the FEC
// decode, so RS ran exclusively on frames that were already intact and could
// never repair anything. This pins the corrected behaviour: a frame with
// damage inside the RS-protected payload is recovered byte-exact and counted
// as a rescue, damage beyond RS capacity is still rejected, and an undamaged
// frame is not counted as rescued.
static void test_fec_rescue() {
    sdr::ReedSolomon rs;
    std::vector<uint8_t> payload(600);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + 3);

    struct Case { int nerr; bool recover; bool rescued; };
    const Case cases[] = {
        {0,  true,  false},   // intact: recovered, but not a "rescue"
        {1,  true,  true },
        {8,  true,  true },
        {16, true,  true },
        {40, false, false},   // beyond RS capacity
    };

    for (const auto& c : cases) {
        sdr::Framer fr;
        auto frame = fr.encode(payload.data(), payload.size(), sdr::FL_FEC,
                               sdr::ModCode::QPSK, sdr::mhzToBw(1), 0x1234, 42, &rs, nullptr);
        const size_t start = sdr::PREAMBLE_LEN + sdr::HEADER_SIZE + 10;
        for (int k = 0; k < c.nerr && start + size_t(k) * 7 < frame.size() - 8; ++k)
            frame[start + size_t(k) * 7] ^= 0xFF;

        sdr::Deframer df;
        bool exact = false;
        for (uint8_t b : frame) {
            auto r = df.push(b, &rs, nullptr);
            if (r) { exact = (r->payload == payload); break; }
        }
        assert(exact == c.recover);
        assert((df.fecRescued() > 0) == c.rescued);
    }
    std::cout << "  [framing] Reed-Solomon rescues corrupted frames: PASS\n";
}

static void test_maximum_fec_codewords() {
    sdr::ReedSolomon rs;
    assert(sdr::ReedSolomon::encodedSize(sdr::MAX_PAYLOAD) == sdr::MAX_CODED_PAYLOAD);
    for (size_t length : {size_t(223),size_t(224),size_t(1338),size_t(1339),sdr::MAX_PAYLOAD}) {
        std::vector<uint8_t> payload(length);
        for (size_t i=0;i<length;++i) payload[i]=static_cast<uint8_t>(i*37+11);
        for (unsigned errors : {0u, 16u, 40u}) {
            auto wire=sdr::Framer().encode(payload.data(),payload.size(),sdr::FL_FEC,
                                           sdr::ModCode::QAM64,sdr::mhzToBw(1),7,99,&rs,nullptr);
            const size_t last_block=sdr::PREAMBLE_LEN+sdr::HEADER_SIZE+
                                    sdr::ReedSolomon::encodedSize(length)-255;
            for(unsigned i=0;i<errors;++i) wire[last_block+i]^=0x91;
            sdr::Deframer decoder;
            std::optional<sdr::DecodedFrame> decoded;
            for(auto byte:wire) if(auto frame=decoder.push(byte,&rs,nullptr))decoded=std::move(frame);
            if(errors<=16) {
                assert(decoded && decoded->payload==payload);
                assert(decoder.fecFailed()==0);
                assert(decoder.fecRescued()==(errors ? 1u : 0u));
            } else {
                assert(!decoded);
                assert(decoder.fecFailed()==1);
            }
        }
    }
    std::cout << "  [framing] maximum FEC codewords and repair bounds: PASS\n";
}

static void test_invalid_codeword_with_valid_outer_crc() {
    sdr::ReedSolomon rs;
    std::vector<uint8_t> payload(223,0x5a);
    auto wire=sdr::Framer().encode(payload.data(),payload.size(),sdr::FL_FEC,
                                  sdr::ModCode::QPSK,sdr::mhzToBw(1),7,101,&rs,nullptr);
    const size_t start=sdr::PREAMBLE_LEN+sdr::HEADER_SIZE;
    for(size_t i=0;i<60;++i) wire[start+i]^=static_cast<uint8_t>(i*13+1);
    const size_t crc_at=start+sdr::ReedSolomon::encodedSize(payload.size());
    uint32_t crc=0xffffffffu;
    for(size_t i=sdr::PREAMBLE_LEN;i<crc_at;++i) {
        crc^=wire[i];
        for(int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u & -(crc&1u));
    }
    crc=~crc;
    for(unsigned i=0;i<4;++i) wire[crc_at+i]=static_cast<uint8_t>(crc>>(8*i));
    sdr::Deframer decoder;
    for(auto byte:wire) assert(!decoder.push(byte,&rs,nullptr));
    assert(decoder.fecFailed()==1);
    std::cout << "  [framing] invalid RS codeword with valid outer CRC rejected: PASS\n";
}

void run_framing() {
    std::cout << "[framing tests]\n";
    test_roundtrip();
    test_corrupt_crc();
    test_large_payload();
    test_multi_frame();
    test_fec_rescue();
    test_maximum_fec_codewords();
    test_invalid_codeword_with_valid_outer_crc();
    std::cout << "[framing tests] ALL PASS\n\n";
}
