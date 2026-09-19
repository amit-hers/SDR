#include "sdr/framing/DmaBlockAggregator.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/OffsetDeframer.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace sdr;

static std::vector<uint8_t> dmaPayload(size_t n, uint8_t seed) {
    std::vector<uint8_t> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(seed + i * 17);
    return p;
}

static std::vector<OffsetFrame> decodeBlocks(const std::vector<CompletedDmaBlock>& blocks) {
    OffsetDeframer d;
    std::vector<OffsetFrame> out;
    for (const auto& b : blocks) {
        auto got = d.pushPacket(b.bytes.data(), b.bytes.size());
        for (auto& f : got) out.push_back(std::move(f));
    }
    return out;
}

void run_dma_aggregation() {
    std::printf("[dma_aggregation] complete RF frames share DMA blocks\n");
    Framer framer;

    // Three packets -> one block -> three byte-identical packets, in order.
    {
        DmaBlockAggregator a(4096);
        std::vector<CompletedDmaBlock> blocks;
        std::vector<std::vector<uint8_t>> sent = {
            dmaPayload(64, 1), dmaPayload(128, 2), dmaPayload(512, 3)};
        uint32_t seq = 0;
        for (const auto& p : sent) {
            auto w = framer.encode(p, 0, ModCode::QPSK, BwCode::BW_5, 10, seq++);
            assert(a.addFrame(w, p.size(), true, blocks));
        }
        a.flush(blocks);
        assert(blocks.size() == 1);
        assert(blocks[0].data_packets == 3);
        auto got = decodeBlocks(blocks);
        assert(got.size() == sent.size());
        for (size_t i = 0; i < sent.size(); ++i) assert(got[i].payload == sent[i]);
    }

    // A pending packet that does not fit flushes the prior block, then enters
    // the next one. Nothing is lost and no frame crosses a DMA boundary.
    {
        DmaBlockAggregator a(1800);
        std::vector<CompletedDmaBlock> blocks;
        std::vector<std::vector<uint8_t>> sent = {
            dmaPayload(64, 1), dmaPayload(128, 2), dmaPayload(512, 3), dmaPayload(1400, 4)};
        uint32_t seq = 20;
        for (const auto& p : sent) {
            auto w = framer.encode(p, 0, ModCode::QPSK, BwCode::BW_5, 10, seq++);
            assert(a.addFrame(w, p.size(), true, blocks));
        }
        a.flush(blocks);
        assert(blocks.size() == 2);
        auto got = decodeBlocks(blocks);
        assert(got.size() == sent.size());
        for (size_t i = 0; i < sent.size(); ++i) assert(got[i].payload == sent[i]);
    }

    // Same sequence from different nodes remains distinct after aggregation.
    {
        DmaBlockAggregator a(1024);
        std::vector<CompletedDmaBlock> blocks;
        auto p1 = dmaPayload(80, 7), p2 = dmaPayload(80, 8);
        auto w1 = framer.encode(p1, 0, ModCode::QPSK, BwCode::BW_5, 1, 5);
        auto w2 = framer.encode(p2, 0, ModCode::QPSK, BwCode::BW_5, 2, 5);
        assert(a.addFrame(w1, p1.size(), true, blocks));
        assert(a.addFrame(w2, p2.size(), true, blocks));
        a.flush(blocks);
        auto got = decodeBlocks(blocks);
        assert(got.size() == 2);
        assert(got[0].node_id == 1 && got[1].node_id == 2);
    }

    // Impossible entries fail explicitly; corrupt/truncated wire metadata is
    // rejected by the existing bounds-checked Deframer and yields no payload.
    {
        DmaBlockAggregator a(128);
        std::vector<CompletedDmaBlock> blocks;
        assert(!a.addFrame({}, 0, true, blocks));
        assert(!a.addFrame(std::vector<uint8_t>(129, 0xAA), 129, true, blocks));

        auto p = dmaPayload(32, 9);
        auto bad = framer.encode(p, 0, ModCode::QPSK, BwCode::BW_5, 3, 9);
        bad[PREAMBLE_LEN] ^= 0xFF; // corrupt sync/magic
        DmaBlockAggregator b(256);
        assert(b.addFrame(bad, p.size(), true, blocks));
        b.flush(blocks);
        assert(decodeBlocks(blocks).empty());
    }
}
