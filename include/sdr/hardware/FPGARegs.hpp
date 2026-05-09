#pragma once
// FPGARegs.hpp — Memory-mapped access to HLS IP AXI4-Lite registers
//
// Usage (on PlutoSDR Linux, must run as root):
//
//   sdr::FPGARegs regs;
//   if (regs.open()) {
//       float power_dbfs = regs.rssi_power_dbfs();
//       regs.set_rx_gain(0.8f);           // 80% digital gain
//       regs.set_pa_gate(28000);          // gate TX if |I| or |Q| > 28000
//       uint32_t frames = regs.sync_match_count();
//   }
//
// All register addresses match pluto_sdr_bd.tcl assignments.

#include <cstdint>
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace sdr {

class FPGARegs {
public:
    // AXI4-Lite base addresses (from pluto_sdr_bd.tcl)
    static constexpr uint32_t BASE_RSSI      = 0x43C00000;
    static constexpr uint32_t BASE_GAIN_RX   = 0x43C10000;
    static constexpr uint32_t BASE_GAIN_TX   = 0x43C20000;
    static constexpr uint32_t BASE_DEMOD     = 0x43C30000;
    static constexpr uint32_t BASE_SYNC      = 0x43C40000;
    static constexpr uint32_t MAP_SIZE       = 0x10000;    // 64 KB covers all 5 blocks

    // rssi_meter offsets
    static constexpr uint32_t RSSI_WINDOW    = 0x10;
    static constexpr uint32_t RSSI_POWER     = 0x18;
    static constexpr uint32_t RSSI_VALID     = 0x20;
    static constexpr uint32_t RSSI_PEAK      = 0x28;

    // gain_block offsets (same for RX and TX)
    static constexpr uint32_t GAIN_VALUE     = 0x10;   // Q0.12
    static constexpr uint32_t GAIN_ENABLED   = 0x18;
    static constexpr uint32_t GAIN_AMP_LIMIT = 0x20;
    static constexpr uint32_t GAIN_CLIPS     = 0x28;
    static constexpr uint32_t GAIN_GATES     = 0x30;

    // sync_detector offsets
    static constexpr uint32_t SYNC_WORD      = 0x10;
    static constexpr uint32_t SYNC_MATCHES   = 0x18;
    static constexpr uint32_t SYNC_DROPS     = 0x20;

    // qpsk_demod offsets
    static constexpr uint32_t DEMOD_ENABLED  = 0x10;
    static constexpr uint32_t DEMOD_LOCKS    = 0x18;

    FPGARegs() = default;
    ~FPGARegs() { close(); }
    FPGARegs(const FPGARegs&) = delete;

    bool open() {
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd_ < 0) return false;

        auto map = [&](uint32_t base) -> volatile uint32_t* {
            void* p = mmap(nullptr, MAP_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd_, base);
            return (p == MAP_FAILED) ? nullptr
                 : reinterpret_cast<volatile uint32_t*>(p);
        };

        rssi_   = map(BASE_RSSI);
        gain_rx_= map(BASE_GAIN_RX);
        gain_tx_= map(BASE_GAIN_TX);
        demod_  = map(BASE_DEMOD);
        sync_   = map(BASE_SYNC);

        return rssi_ && gain_rx_ && gain_tx_ && demod_ && sync_;
    }

    void close() {
        auto unmap = [](volatile uint32_t* p) {
            if (p) munmap(const_cast<uint32_t*>(
                reinterpret_cast<const uint32_t*>(p)), MAP_SIZE);
        };
        unmap(rssi_); unmap(gain_rx_); unmap(gain_tx_);
        unmap(demod_); unmap(sync_);
        rssi_ = gain_rx_ = gain_tx_ = demod_ = sync_ = nullptr;
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const { return fd_ >= 0; }

    // ── RSSI meter ───────────────────────────────────────────────────

    // Returns mean power in dBFS (0 dBFS = full-scale 32767 ADC counts)
    float rssi_power_dbfs() const {
        if (!rssi_) return -100.f;
        uint32_t p = reg(rssi_, RSSI_POWER);
        if (p == 0) return -100.f;
        // power_accum = mean(I²+Q²) over window; full-scale = 32767² = 1.07e9
        return 10.f * std::log10(static_cast<float>(p) / 1.07e9f);
    }

    bool rssi_valid() const {
        return rssi_ && reg(rssi_, RSSI_VALID) != 0;
    }

    void set_rssi_window(uint32_t log2_n) {
        if (rssi_) reg(rssi_, RSSI_WINDOW) = log2_n;
    }

    // ── Digital gain ─────────────────────────────────────────────────

    // gain 0.0–1.0 (1.0 = unity, 0.0 = mute)
    void set_rx_gain(float g) {
        if (gain_rx_) {
            uint32_t q12 = static_cast<uint32_t>(g * 4096.f);
            if (q12 > 4095) q12 = 4095;
            reg(gain_rx_, GAIN_VALUE)   = q12;
            reg(gain_rx_, GAIN_ENABLED) = 1;
        }
    }

    void set_tx_gain(float g) {
        if (gain_tx_) {
            uint32_t q12 = static_cast<uint32_t>(g * 4096.f);
            if (q12 > 4095) q12 = 4095;
            reg(gain_tx_, GAIN_VALUE)   = q12;
            reg(gain_tx_, GAIN_ENABLED) = 1;
        }
    }

    // PA safety gate: zero TX if |I| or |Q| exceeds threshold (0 = disable)
    void set_pa_gate(uint16_t threshold) {
        if (gain_tx_) reg(gain_tx_, GAIN_AMP_LIMIT) = threshold;
    }

    uint32_t tx_clip_count() const {
        return gain_tx_ ? reg(gain_tx_, GAIN_CLIPS) : 0;
    }

    uint32_t pa_gate_trips() const {
        return gain_tx_ ? reg(gain_tx_, GAIN_GATES) : 0;
    }

    // ── Sync detector ────────────────────────────────────────────────

    // Returns frames detected since last call (clears counter on read)
    uint32_t sync_match_count() const {
        return sync_ ? reg(sync_, SYNC_MATCHES) : 0;
    }

    uint32_t sync_drop_count() const {
        return sync_ ? reg(sync_, SYNC_DROPS) : 0;
    }

    void set_sync_word(uint32_t word) {
        if (sync_) reg(sync_, SYNC_WORD) = word;
    }

    // ── Demod control ────────────────────────────────────────────────

    void set_demod_enabled(bool en) {
        if (demod_) reg(demod_, DEMOD_ENABLED) = en ? 1 : 0;
    }

    uint32_t demod_lock_count() const {
        return demod_ ? reg(demod_, DEMOD_LOCKS) : 0;
    }

private:
    int fd_{-1};
    volatile uint32_t* rssi_   {nullptr};
    volatile uint32_t* gain_rx_{nullptr};
    volatile uint32_t* gain_tx_{nullptr};
    volatile uint32_t* demod_  {nullptr};
    volatile uint32_t* sync_   {nullptr};

    static uint32_t& reg(volatile uint32_t* base, uint32_t off) {
        return const_cast<uint32_t&>(
            reinterpret_cast<const volatile uint32_t*>(base)[off / 4]);
    }
    static uint32_t reg(const volatile uint32_t* base, uint32_t off) {
        return base[off / 4];
    }
};

} // namespace sdr
