#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Frame.hpp"

namespace sdr {

// A DMA block is the aggregate container. Entries retain the existing RF
// Frame header (sync/version/node/sequence/length/CRC), so no second wire
// protocol is needed. Padding is never parsed as a terminator: Deframer finds
// and validates complete frame headers and lengths independently.
struct CompletedDmaBlock {
    std::vector<uint8_t> bytes;
    size_t frame_count = 0;
    size_t data_packets = 0;
    size_t payload_bytes = 0;
    size_t padding_bytes = 0;
};

class DmaBlockAggregator {
public:
    explicit DmaBlockAggregator(size_t block_size) : block_size_(block_size) {
        if (block_size_ == 0) throw std::invalid_argument("DMA block size is zero");
        bytes_.reserve(block_size_);
    }

    // Adds one already encoded, independently bounds-checkable RF frame.
    // If it does not fit, the prior block is emitted first and the pending
    // frame is then inserted into a fresh block; it is never silently dropped.
    bool addFrame(const std::vector<uint8_t>& wire, size_t payload_bytes,
                  bool is_data, std::vector<CompletedDmaBlock>& out) {
        if (wire.empty() || wire.size() > block_size_) return false;
        if (!bytes_.empty() && bytes_.size() + wire.size() > block_size_)
            out.push_back(finish());
        bytes_.insert(bytes_.end(), wire.begin(), wire.end());
        ++frame_count_;
        if (is_data) {
            ++data_packets_;
            payload_bytes_ += payload_bytes;
        }
        return true;
    }

    bool empty() const { return bytes_.empty(); }
    size_t used() const { return bytes_.size(); }
    size_t remaining() const { return block_size_ - bytes_.size(); }

    void flush(std::vector<CompletedDmaBlock>& out) {
        if (!bytes_.empty()) out.push_back(finish());
    }

private:
    CompletedDmaBlock finish() {
        CompletedDmaBlock b;
        b.frame_count = frame_count_;
        b.data_packets = data_packets_;
        b.payload_bytes = payload_bytes_;
        b.padding_bytes = block_size_ - bytes_.size();
        // Pad with the preamble byte, NOT zero. The link is differentially
        // encoded: a zero dibit is "no phase change", so a zero tail goes on
        // the air as an unmodulated tone and the receiver's timing loop has
        // nothing to track until the next block. Measured: 88% loss of dense
        // 1200-byte UDP while sparse pings passed -- each data block's zero
        // tail derailed the demodulator for the next block's first frames,
        // and idle blocks (all keepalives, no padding) never showed it.
        // 0xAA is a constant rotation every symbol, the same training pattern
        // the frame preamble uses, so the tail keeps the loops fed instead.
        // The deframer hunts for the sync word and never parses padding.
        bytes_.resize(block_size_, PREAMBLE_BYTE);
        b.bytes = std::move(bytes_);
        bytes_.clear();
        bytes_.reserve(block_size_);
        frame_count_ = data_packets_ = payload_bytes_ = 0;
        return b;
    }

    size_t block_size_;
    std::vector<uint8_t> bytes_;
    size_t frame_count_ = 0;
    size_t data_packets_ = 0;
    size_t payload_bytes_ = 0;
};

} // namespace sdr
