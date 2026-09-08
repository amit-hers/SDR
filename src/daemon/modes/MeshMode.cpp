#include "MeshMode.hpp"
#include "sdr/framing/Frame.hpp"
#include <vector>
#include <complex>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace sdr {

MeshMode::MeshMode(const Config& cfg, PlutoSDR& radio)
    : cfg_(cfg), radio_(radio)
{
    tun_  = TUNTAPDevice::create(cfg_.tap_iface, /*tap=*/false);

    // A TUN carries IP packets, not Ethernet frames, so the 14 bytes reserved
    // for an Ethernet header in tap_mtu are not spent here. Using the TAP value
    // would waste them on every packet.
    const int mtu = (cfg_.tap_mtu == 1386) ? static_cast<int>(MAX_PAYLOAD) : cfg_.tap_mtu;
    tun_->setMTU(mtu);

    if (!cfg_.ip_local.empty()) {
        // Refuse a link subnet that overlaps a route this machine already has.
        // The failure is otherwise silent: the address is accepted, and traffic
        // for the peer leaves by the pre-existing route instead of the radio.
        const std::string cidr = cfg_.ip_local + "/" + std::to_string(cfg_.ip_prefix);
        if (TUNTAPDevice::conflictsWithExistingRoute(cidr, cfg_.tap_iface)) {
            throw std::runtime_error(
                "mesh: link subnet " + cidr + " overlaps a route this host already has.\n"
                "       Traffic for the peer would leave by that route instead of the radio,\n"
                "       with nothing reported. Choose a subnet that does not overlap\n"
                "       (172.31.x is usually free where 10.x and 192.168.x are not).");
        }
        // Ask NetworkManager to leave this interface alone BEFORE addressing
        // it. It manages new interfaces by default and will flush an address it
        // did not assign, usually within a second or two of the link appearing.
        // Runtime-only and harmless where NetworkManager is absent.
        if (std::system("command -v nmcli >/dev/null 2>&1") == 0) {
            const std::string cmd = "nmcli device set " + cfg_.tap_iface +
                                    " managed no >/dev/null 2>&1";
            (void)std::system(cmd.c_str());
        }

        tun_->setIPv4(cfg_.ip_local, cfg_.ip_prefix, cfg_.ip_peer);

        // Confirm it stuck. An address that is silently removed leaves the
        // radio looking dead while the real fault is on this host.
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const std::string got = tun_->currentIPv4();
        if (got != cfg_.ip_local) {
            throw std::runtime_error(
                "mesh: " + cfg_.tap_iface + " lost its address immediately after it was set"
                " (now '" + (got.empty() ? std::string("none") : got) + "').\n"
                "       Something on this host is managing the interface -- normally\n"
                "       NetworkManager. Mark it unmanaged and retry:\n"
                "         nmcli device set " + cfg_.tap_iface + " managed no");
        }

        std::cout << "[sdr] " << cfg_.tap_iface << ": " << cfg_.ip_local
                  << "/" << cfg_.ip_prefix;
        if (!cfg_.ip_peer.empty()) std::cout << " peer " << cfg_.ip_peer;
        std::cout << " mtu " << mtu << "\n";

        if (!cfg_.route_via_peer.empty()) {
            const bool ok = tun_->addRoute(cfg_.route_via_peer, cfg_.ip_peer);
            std::cout << "[sdr] route " << cfg_.route_via_peer << " via radio: "
                      << (ok ? "up" : "not added (already present?)") << "\n";
        }
    }
    tx_mod_ = cfg_.txModCode();
    if (cfg_.fec)     fec_ = std::make_unique<ReedSolomon>();
    if (cfg_.encrypt) aes_ = std::make_unique<AESCipher>(cfg_.aes_key_bytes.data());
}

MeshMode::~MeshMode() { stop(); }

void MeshMode::start() {
    if (running_.exchange(true)) return;
    tx_thread_ = std::thread(&MeshMode::txThread, this);
    rx_thread_ = std::thread(&MeshMode::rxThread, this);
}

void MeshMode::stop() {
    running_.store(false);
    if (tx_thread_.joinable()) tx_thread_.join();
    if (rx_thread_.joinable()) rx_thread_.join();
}

void MeshMode::txThread() {
    std::vector<uint8_t>             pkt(MAX_PAYLOAD);
    std::vector<std::complex<float>> iq_syms, iq_shaped;
    std::vector<int16_t>             iq_hw;
    RRCInterp interp(cfg_.samples_per_symbol);

    while (running_.load()) {
        ssize_t n = tun_->read(pkt.data(), pkt.size());
        if (n <= 0) continue;

        uint8_t flags = 0;
        if (cfg_.fec)     flags |= FL_FEC;
        if (cfg_.encrypt) flags |= FL_ENCRYPT;
        uint32_t seq = tx_seq_.fetch_add(1, std::memory_order_relaxed);

        Framer framer;
        auto frame = framer.encode(pkt.data(), static_cast<size_t>(n),
                                   flags, tx_mod_, mhzToBw(cfg_.bw_mhz),
                                   cfg_.node_id_u32, seq, fec_.get(), aes_.get());

        // Acquisition in BPSK, payload in tx_mod_ -- see BridgeMode and
        // Frame.hpp. Modulating the preamble with the payload scheme makes
        // the burst invisible to the receiver's BPSK correlator.
        SplitModem::modulate(frame, tx_mod_, iq_syms);
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

void MeshMode::rxThread() {
    std::vector<int16_t>             iq_hw(IQ_SAMPLES * 2);
    std::vector<std::complex<float>> iq_f, window_buf, offset_buf, iq_timed, iq_syms;
    AGC agc; TimingSync tsync(cfg_.samples_per_symbol); CostasLoop costas;
    DataAidedSync dasync;
    BurstDetector detector;
    const double sample_rate = static_cast<double>(cfg_.bw_mhz) * 1e6 *
                               cfg_.samples_per_symbol;

    while (running_.load()) {
        int n = radio_.rxPull(iq_hw.data(), IQ_SAMPLES);
        if (n <= 0) continue;

        iq_f.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            iq_f[static_cast<size_t>(i)] = {
                iq_hw[static_cast<size_t>(i)*2]   / 2048.f,
                iq_hw[static_cast<size_t>(i)*2+1] / 2048.f };

        // See BridgeMode::rxThread for why: a real transmission is a sparse,
        // short burst inside a mostly-silent batch, so detect and isolate
        // it first rather than running AGC/TimingSync/CostasLoop
        // continuously across the whole batch.
        auto windows = detector.detect(iq_f);
        if (windows.empty()) continue;

        for (const auto& win : windows) {
            window_buf.assign(iq_f.begin() + static_cast<long>(win.start),
                              iq_f.begin() + static_cast<long>(win.end));

            CoarseFreqCorrect::apply(window_buf, sample_rate);

            // See BridgeMode::rxThread: TimingSync's lock depends brittlely
            // on where the window starts relative to the RRC tap grid, so
            // retry at each sub-grid alignment until one yields a frame.
            bool decoded_any = false;
            for (size_t offset = 0; offset < DECODE_OFFSETS && !decoded_any; ++offset) {
                if (offset >= window_buf.size()) break;
                offset_buf.assign(window_buf.begin() + static_cast<long>(offset),
                                  window_buf.end());

                agc.reset(); tsync.reset(); costas.reset();

                agc.process(offset_buf);
                // NOTE: TimingSync is itself an RRC matched-filter + decimator
                // (symsync_crcf expects oversampled input); do not RRCDecim first.
                tsync.process(offset_buf, iq_timed);

                // Least-squares carrier estimate from the known BPSK
                // acquisition symbols, then one open-loop derotation. See
                // BridgeMode::rxThread for the measurements behind this.
                iq_syms = iq_timed;
                auto est = dasync.estimate(iq_timed, 64);
                if (est.ok) DataAidedSync::derotate(iq_syms, est);

                // Payload modulation comes from the frame header, never from
                // local state.
                auto sm = SplitModem::demodulate(iq_syms, fec_ != nullptr,
                                                 PREAMBLE_LEN * 8 + 128);
                if (!sm.complete) continue;

                // Fresh per attempt: a misaligned attempt's garbage would
                // poison a shared Deframer's state machine.
                Deframer deframer;
                for (uint8_t byte : sm.bytes) {
                    auto res = deframer.push(byte, fec_.get(), aes_.get());
                    if (!res) continue;
                    decoded_any = true;
                    ssize_t w = tun_->write(res->payload.data(), res->payload.size());
                    if (w > 0) {
                        stats_.frames_rx_good.fetch_add(1, std::memory_order_relaxed);
                        stats_.bytes_rx.fetch_add(static_cast<uint64_t>(w),
                                                  std::memory_order_relaxed);
                    }
                }
            }
        }
    }
}

} // namespace sdr
