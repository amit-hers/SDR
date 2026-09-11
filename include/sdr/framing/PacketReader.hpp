#pragma once
// Reassemble whole DMA packets from a byte stream that does not preserve them.
//
// The receive path decodes ONE DMA transfer at a time: the byte grid is
// continuous only within a transfer, because the demodulator keeps emitting
// through the gap between packets and the number of bytes lost there is
// arbitrary. Analysing across a boundary was measured to report 22.37% loss
// against a true 0.19%.
//
// sdr_bridge reads from a pipe (iio_readdev's stdout, the only path that
// programs the DMA on this kernel), and a pipe delivers whatever happens to be
// available rather than one buffer per read. A single read therefore returns a
// FRAGMENT. Decoding a fragment as if it were a packet discards the frames at
// both of its ends and reports them as link loss -- exactly the misattribution
// these paths exist to avoid.
//
// readExact() closes that gap: it returns only when a full packet has been
// assembled, or when the stream genuinely ends.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace sdr {

// Read exactly `n` bytes into `buf`, continuing across however the underlying
// stream chose to fragment them.
//
// Returns n on success. Returns a short count ONLY at genuine end of stream
// (the writer closed), or -1 on a real error. EINTR is retried rather than
// treated as either.
inline ssize_t readExact(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = ::read(fd, buf + got, n - got);
        if (k == 0) break;                 // writer closed: genuine EOF
        if (k < 0) {
            if (errno == EINTR) continue;  // not an error, and not EOF
            return got > 0 ? static_cast<ssize_t>(got) : -1;
        }
        got += static_cast<size_t>(k);
    }
    return static_cast<ssize_t>(got);
}

} // namespace sdr
