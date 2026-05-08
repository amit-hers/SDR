#include "P2PMode.hpp"
#include "sdr/framing/Frame.hpp"
#include <vector>
#include <complex>

namespace sdr {

P2PMode::P2PMode(const Config& cfg, PlutoSDR& radio)
    : cfg_(cfg), radio_(radio)
{
    modem_ = std::make_unique<Modem>(ModScheme::QPSK);
    if (cfg_.fec)     fec_ = std::make_unique<ReedSolomon>();
    if (cfg_.encrypt) aes_ = std::make_unique<AESCipher>(cfg_.aes_key_bytes.data());
    udp_ = UDPSocket::bind(P2P_PORT);
}

P2PMode::~P2PMode() { stop(); }

void P2PMode::start() {
    if (running_.exchange(true)) return;
    if (cfg_.mode == "p2p-tx")
        active_thread_ = std::thread(&P2PMode::txThread, this);
    else
        active_thread_ = std::thread(&P2PMode::rxThread, this);
}

void P2PMode::stop() {
    running_.store(false);
    if (active_thread_.joinable()) active_thread_.join();
}

void P2PMode::txThread() {
    std::vector<uint8_t>             pkt(MAX_PAYLOAD);
    std::vector<std::complex<float>> iq_syms, iq_shaped;
    std::vector<int16_t>             iq_hw;
    RRCInterp interp;

    while (running_.load()) {
        ssize_t n = udp_->read(pkt.data(), pkt.size());
        if (n <= 0) continue;

        uint8_t flags = 0;
        if (cfg_.fec)     flags |= FL_FEC;
        if (cfg_.encrypt) flags |= FL_ENCRYPT;
        uint32_t seq = tx_seq_.fetch_add(1, std::memory_order_relaxed);

        Framer framer;
        auto frame = framer.encode(pkt.data(), static_cast<size_t>(n),
                                   flags, ModCode::QPSK, mhzToBw(cfg_.bw_mhz),
                                   cfg_.node_id_u32, seq, fec_.get(), aes_.get());

        modem_->modulate(frame.data(), static_cast<int>(frame.size()), iq_syms);
        interp.process(iq_syms, iq_shaped);

        iq_hw.resize(iq_shaped.size() * 2);
        for (size_t i = 0; i < iq_shaped.size(); ++i) {
            iq_hw[i*2]   = static_cast<int16_t>(iq_shaped[i].real() * 2047.f);
            iq_hw[i*2+1] = static_cast<int16_t>(iq_shaped[i].imag() * 2047.f);
        }
        radio_.txPush(iq_hw.data(), iq_shaped.size());
        stats_.frames_tx.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_tx .fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    }
}

void P2PMode::rxThread() {
    std::vector<int16_t>             iq_hw(IQ_SAMPLES * 2);
    std::vector<std::complex<float>> iq_f, iq_dec, iq_timed, iq_syms;
    AGC agc; RRCDecim decim; TimingSync tsync; CostasLoop costas;
    Deframer deframer;

    while (running_.load()) {
        int n = radio_.rxPull(iq_hw.data(), IQ_SAMPLES);
        if (n <= 0) continue;

        iq_f.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            iq_f[static_cast<size_t>(i)] = {
                iq_hw[static_cast<size_t>(i)*2]   / 2048.f,
                iq_hw[static_cast<size_t>(i)*2+1] / 2048.f };

        agc.process(iq_f);
        decim.process(iq_f, iq_dec);
        tsync.process(iq_dec, iq_timed);
        costas.process(iq_timed, iq_syms);

        std::vector<uint8_t> bits;
        modem_->demodulate(iq_syms.data(), static_cast<int>(iq_syms.size()), bits);

        for (uint8_t byte : bits) {
            auto res = deframer.push(byte, fec_.get(), aes_.get());
            if (!res) continue;
            udp_->write(res->payload.data(), res->payload.size());
            stats_.frames_rx_good.fetch_add(1, std::memory_order_relaxed);
            stats_.bytes_rx.fetch_add(static_cast<uint64_t>(res->payload.size()),
                                      std::memory_order_relaxed);
        }
    }
}

} // namespace sdr
