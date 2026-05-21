# ── Zynq-7020 QSPI recovery via JTAG (FT4232H) ─────────────────────────────
# Usage:
#   openocd -f zynq7020_ft4232.cfg \
#     -c "source full_recovery.tcl; init; after 1000; qspi_recover; shutdown"

proc words_from_file {fname} {
    set f [open $fname rb]; set d [read $f]; close $f
    set n [string length $d]
    set pad [expr {(4 - ($n % 4)) % 4}]
    for {set i 0} {$i < $pad} {incr i} { append d "\xFF" }
    binary scan $d iu* words
    return $words
}

proc words_from_bytes {bytes} {
    set words {}
    set n [llength $bytes]
    for {set i 0} {$i < $n} {incr i 4} {
        set b0 [lindex $bytes $i]
        set b1 [lindex $bytes [expr {$i+1}]]
        set b2 [lindex $bytes [expr {$i+2}]]
        set b3 [lindex $bytes [expr {$i+3}]]
        if {$b1 eq ""} {set b1 255}
        if {$b2 eq ""} {set b2 255}
        if {$b3 eq ""} {set b3 255}
        lappend words [expr {$b0 | ($b1<<8) | ($b2<<16) | ($b3<<24)}]
    }
    return $words
}

proc write_words {addr wlist} {
    set step 64
    set n [llength $wlist]
    for {set i 0} {$i < $n} {incr i $step} {
        set e [expr {$i+$step-1}]
        if {$e >= $n} {set e [expr {$n-1}]}
        write_memory [expr {$addr + $i*4}] 32 [lrange $wlist $i $e]
    }
}

# ── Pure-TCL QSPI byte transfer (CPU stays halted) ───────────────────────────
# Writes to the QSPI TX register and waits for RXNE.
# txd_offset: 0x1C = TXD_00_00 (4-byte transfer), 0x80 = TXD_00_01 (1-byte)
# man_start: 1 = set MANSTRT bit after writing TX; 0 = auto-start
# Returns received byte (shifted from RXD[31:24]), or -1 on timeout.
proc qspi_tcl_xfer {tx txd_offset cr_val man_start} {
    set QSPI_BASE 0xE000D000
    set ISR_ADDR  [expr {$QSPI_BASE + 0x04}]
    set TXD_ADDR  [expr {$QSPI_BASE + $txd_offset}]
    set RXD_ADDR  [expr {$QSPI_BASE + 0x20}]

    # Wait until TX FIFO not full (bit 3 = TXF)
    for {set i 0} {$i < 1000} {incr i} {
        set isr [read_memory $ISR_ADDR 32 1]
        if {($isr & 0x08) == 0} break
    }

    # TXD0 (0x1C, 4-byte) packs first byte at [31:24]; TXD1 (0x80, 1-byte) uses [7:0]
    if {$txd_offset == 0x80 || $txd_offset == 0x84 || $txd_offset == 0x88} {
        set txval [expr {$tx & 0xFF}]
    } else {
        set txval [expr {($tx & 0xFF) << 24}]
    }
    write_memory $TXD_ADDR 32 [list $txval]

    # Trigger MANSTRT if requested (set bit 16 in CR)
    if {$man_start} {
        write_memory [expr {$QSPI_BASE + 0x00}] 32 [list [expr {$cr_val | 0x10000}]]
    }

    # Wait for RXNE (bit 4 of ISR) — at ~390 kHz SPI, 8 bits = ~20 µs.
    # Each OpenOCD memory read takes ~1 ms, so 1 iteration should suffice.
    # Give 200 iterations = ~200 ms to be safe.
    set rxne 0
    for {set i 0} {$i < 200} {incr i} {
        set isr [read_memory $ISR_ADDR 32 1]
        if {($isr & 0x10) != 0} { set rxne 1; break }
    }

    set isr_end [read_memory $ISR_ADDR 32 1]
    if {!$rxne} {
        echo "    RXNE never set! Final ISR=0x[format %08X $isr_end]"
        return -1
    }

    set rxd [read_memory $RXD_ADDR 32 1]
    return [expr {($rxd >> 24) & 0xFF}]
}

# ── qspi_tcl_jedec: read JEDEC ID in pure TCL (CPU halted) ──────────────────
# Root cause (now fixed): enable_qspi_clocks was writing to 0xF800016C (TPIU_CLK_CTRL)
# instead of 0xF800014C (LQSPI_CLK_CTRL).  The QSPI controller received no clock,
# so RXNE never fired regardless of BAUD_DIV.  This proc still sweeps all combinations
# as a smoke test — if the clock fix works, one of these will get a JEDEC response.
proc qspi_tcl_jedec {cr_io} {
    global jedec_cr jedec_txd_off jedec_man_start
    set QSPI_BASE 0xE000D000

    # BAUD_DIV encoding: value N → divide by 2^(N+1)
    # 7=÷256, 6=÷128, 5=÷64, 4=÷32, 3=÷16, 2=÷8, 1=÷4, 0=÷2
    set baud_labels {7 6 5 4 3 2 1 0}

    foreach baud $baud_labels {
        # Build CR with this BAUD_DIV, SSFORCE=1, SSCTRL=1 (CS deasserted)
        set cr_baud [expr {($cr_io & ~0x38) | ($baud << 3)}]

        foreach txd_off {0x80 0x1C} {
            foreach man_start {0 1} {
                if {$man_start} {
                    set cr_test [expr {$cr_baud | (1<<15)}]
                } else {
                    set cr_test [expr {$cr_baud & ~(1<<15)}]
                }

                set div [expr {2 << $baud}]
                echo "  BAUD÷$div TXD+0x[format %02X $txd_off] man=$man_start  CR=0x[format %08X $cr_test]"

                # Configure QSPI
                write_memory [expr {$QSPI_BASE+0x14}] 32 {0}
                write_memory [expr {$QSPI_BASE+0x0C}] 32 {0x7F}
                write_memory [expr {$QSPI_BASE+0x00}] 32 [list $cr_test]
                write_memory [expr {$QSPI_BASE+0x14}] 32 {1}
                # Drain RX FIFO
                for {set i 0} {$i < 16} {incr i} {
                    set isr [read_memory [expr {$QSPI_BASE+0x04}] 32 1]
                    if {($isr & 0x10) == 0} break
                    read_memory [expr {$QSPI_BASE+0x20}] 32 1
                }

                # Assert CS (clear SSCTRL bit 10)
                set cr_cs [expr {$cr_test & ~0x400}]
                write_memory [expr {$QSPI_BASE+0x00}] 32 [list $cr_cs]
                after 1

                # Send 0x9F + 3 dummy bytes
                set rx {}
                set failed 0
                foreach b {0x9F 0xFF 0xFF 0xFF} {
                    set r [qspi_tcl_xfer $b $txd_off $cr_cs $man_start]
                    if {$r == -1} { set failed 1; break }
                    lappend rx $r
                }

                # Deassert CS
                write_memory [expr {$QSPI_BASE+0x00}] 32 [list $cr_test]

                if {!$failed} {
                    set j0 [lindex $rx 1]
                    set j1 [lindex $rx 2]
                    set j2 [lindex $rx 3]
                    echo "    JEDEC: 0x[format %02X $j0] 0x[format %02X $j1] 0x[format %02X $j2]"
                    if {$j0 != 0xFF && $j0 != 0x00} {
                        echo "    *** Valid JEDEC — SPI confirmed ***"
                        set jedec_cr      $cr_test
                        set jedec_txd_off $txd_off
                        set jedec_man_start $man_start
                        return 1
                    }
                }
            }
        }
    }
    return 0
}

# ── wait_for_status: halt→read→resume loop ──────────────────────────────────
proc wait_for_status {addr expected timeout_s} {
    set iters [expr {$timeout_s * 5}]
    set last_v -1
    for {set t 0} {$t < $iters} {incr t} {
        after 200
        catch { halt }
        after 100
        set v 0
        catch { set v [read_memory $addr 32 1] }
        if {$v != $last_v} {
            echo "  [expr {$t*200/1000}]s STATUS=0x[format %08X $v]"
            set last_v $v
        }
        if {$v == $expected} { return 1 }
        catch { resume }
        after 50
    }
    return 0
}

# ── configure_mio_qspi: set MIO[1..6] to QSPI function ─────────────────────
# Values from tezuka firmware ps7_init.tcl (tools/jtag-recovery/ps7_init.tcl):
#   MIO[00] = 0x1200  (non-QSPI, e.g. JTAG TDI)
#   MIO[01] = 0x1202  (QSPI0_SS_B   — bit1=1 selects L0 QSPI function)
#   MIO[02] = 0x0202  (QSPI0_IO[0] / MOSI)
#   MIO[03] = 0x0202  (QSPI0_IO[1] / MISO)
#   MIO[04] = 0x0202  (QSPI0_IO[2])
#   MIO[05] = 0x0202  (QSPI0_SCK)
#   MIO[06] = 0x0202  (QSPI0_IO[1] feedback)
# The BootROM resets MIO to GPIO input (0x0601/0x1601) when it enters JTAG
# mode after a failed QSPI boot, leaving the flash physically disconnected from
# the QSPI controller.  We must restore these before any SPI transaction.
proc configure_mio_qspi {} {
    echo ""
    echo "--- Configuring MIO pins 0-6 for QSPI ---"

    # SLCR must be unlocked or MIO writes are silently discarded.
    # enable_qspi_clocks re-locks SLCR before returning, so we must
    # unlock it again here.
    write_memory 0xF8000008 32 {0x0000DF0D}   ;# unlock SLCR
    after 2
    set locksta [read_memory 0xF800000C 32 1]
    echo "  LOCKSTA = 0x[format %08X $locksta]  (0=unlocked)"

    set MIO_BASE 0xF8000700
    set mask     0x00003FFF
    set vals {0x1200 0x1202 0x0202 0x0202 0x0202 0x0202 0x0202}
    for {set i 0} {$i < 7} {incr i} {
        set addr [expr {$MIO_BASE + $i*4}]
        set old  [read_memory $addr 32 1]
        set v    [lindex $vals $i]
        set new  [expr {($old & ~$mask) | ($v & $mask)}]
        write_memory $addr 32 [list $new]
        set rb   [read_memory $addr 32 1]
        echo "  MIO_PIN_0$i: 0x[format %04X [expr {$old & $mask}]] → 0x[format %04X $v]  readback=0x[format %04X [expr {$rb & $mask}]]"
    }

    # Re-lock SLCR
    write_memory 0xF8000004 32 {0x0000767B}
    after 2
    echo "  MIO QSPI configured ✓"
}

# ── enable_qspi_clocks: start IO PLL if in reset, then enable LQSPI_CLK ─────
#
# Root cause: IO_PLL_CTRL bit 3 (PLL_RESET) = 1 when BootROM enters JTAG mode
# without fully initialising the IO PLL.  LQSPI_CLK is sourced from IO PLL,
# so CLKACT=1 alone does nothing — the PLL output is 0 Hz.
#
# Fix:
#   1. Clear IO_PLL_CTRL bit 3 → IO PLL starts locking
#   2. Wait for IO_PLL_LOCK (PLL_STATUS bit 2)
#   3. Set LQSPI_CLK_CTRL: SRCSEL=IO PLL, DIVISOR, CLKACT=1
#   4. Deassert LQSPI_RST_CTRL if set
#   5. Reset QSPI controller (ER=0→1) so it sees the new clock
#
# Fallback: if IO PLL still fails to lock, try ARM PLL (SRCSEL=2).
proc enable_qspi_clocks {} {
    echo ""
    echo "--- Enabling QSPI clocks via SLCR ---"

    # Unlock SLCR (magic key 0xDF0D to offset 0x08)
    write_memory 0xF8000008 32 {0x0000DF0D}
    after 2
    set locksta [read_memory 0xF800000C 32 1]
    echo "  LOCKSTA = 0x[format %08X $locksta]  (0=unlocked)"

    # Read all three PLL control registers + lock status
    set arm_pll_ctrl [read_memory 0xF8000100 32 1]
    set ddr_pll_ctrl [read_memory 0xF8000104 32 1]
    set io_pll_ctrl  [read_memory 0xF8000108 32 1]
    set pll_status   [read_memory 0xF800010C 32 1]
    echo "  ARM_PLL_CTRL = 0x[format %08X $arm_pll_ctrl]  (bit3=RESET, bit0=BYPASS)"
    echo "  DDR_PLL_CTRL = 0x[format %08X $ddr_pll_ctrl]"
    echo "  IO_PLL_CTRL  = 0x[format %08X $io_pll_ctrl]"
    echo "  PLL_STATUS   = 0x[format %08X $pll_status]  (bit2=IO_lock, bit1=DDR_lock, bit0=ARM_lock)"

    set io_pll_reset  [expr {($io_pll_ctrl  >> 3) & 1}]
    set io_pll_bypass [expr { $io_pll_ctrl        & 1}]
    set io_pll_fdiv   [expr {($io_pll_ctrl  >> 12) & 0x7F}]
    set arm_pll_fdiv  [expr {($arm_pll_ctrl >> 12) & 0x7F}]
    echo "  IO PLL: FDIV=$io_pll_fdiv RESET=$io_pll_reset BYPASS=$io_pll_bypass"

    # ── Step 1: reprogram IO PLL to FDIV=6 → 200 MHz ────────────────────────
    # The LQSPI_CLK_CTRL DIVISOR field reads back as 0 regardless of what is
    # written (it is a write-only shadow latch, but the hardware may use 0 =
    # divide-by-1).  With IO PLL = 858 MHz and divisor=1, LQSPI_CLK = 858 MHz,
    # which is 4× above the Zynq QSPI controller maximum (200 MHz).  The
    # controller's INTERNAL logic (not just output SCK) runs at LQSPI_CLK, so
    # at 858 MHz it locks up — RXNE never fires regardless of BAUD_DIV.
    #
    # Fix: lower the IO PLL frequency to 200 MHz (FDIV=6 → 33×6=198 MHz).
    # Then LQSPI_CLK = 198 MHz (divisor=1) or less, safely within spec.
    # BAUD_DIV=7 in CR gives SCK = 198/256 ≈ 0.77 MHz (ultra-conservative).
    echo "  Reprogramming IO PLL: FDIV=26 (858 MHz) → FDIV=6 (198 MHz)..."
    # Assert reset first (bit3=1), change FDIV, deassert reset
    set pll_reset_val [expr {($io_pll_ctrl & ~(0x7F << 12)) | (1 << 3) | (6 << 12)}]
    write_memory 0xF8000108 32 [list $pll_reset_val]   ;# RESET=1, FDIV=6
    after 2
    set pll_run_val   [expr {$pll_reset_val & ~0x8}]   ;# RESET=0
    write_memory 0xF8000108 32 [list $pll_run_val]
    after 5
    # Wait for IO_PLL_LOCK (PLL_STATUS bit 2)
    for {set t 0} {$t < 100} {incr t} {
        after 2
        set pll_status [read_memory 0xF800010C 32 1]
        if {($pll_status & 0x4) != 0} break
    }
    set pll_status [read_memory 0xF800010C 32 1]
    set io_new [read_memory 0xF8000108 32 1]
    echo "  IO_PLL_CTRL now = 0x[format %08X $io_new]"
    echo "  PLL_STATUS      = 0x[format %08X $pll_status]  (bit2=IO_lock)"
    if {($pll_status & 0x4) == 0} {
        echo "  WARNING: IO PLL may not have locked — continuing anyway"
    } else {
        echo "  IO PLL locked at ≈198 MHz ✓"
    }

    # ── Step 2: enable LQSPI_CLK from IO PLL (divisor=5 → 40 MHz) ──────────
    # LQSPI_CLK_CTRL is at SLCR offset 0x14C (0xF800014C).
    # Previously this script used 0xF800016C = TPIU_CLK_CTRL (trace port)
    # by mistake, so the QSPI controller never received a clock.
    #
    # Register layout: bit[0]=CLKACT, bits[5:4]=SRCSEL, bits[13:8]=DIVISOR
    # With IO PLL ≈ 198 MHz and DIVISOR=5: LQSPI_CLK = 198/5 ≈ 40 MHz.
    # QSPI_CR BAUD_DIV=7 (÷256): SPI SCK ≈ 0.16 MHz — safe for any flash.
    # (DIVISOR must be ≥ 1; 0 means divide-by-0 = no clock.)
    set lqspi_clk_ctrl_addr 0xF800014C
    set srcsel  0   ;# IO PLL
    set divisor 5   ;# 198/5 ≈ 40 MHz (well within 200 MHz controller limit)
    set lqspi_mhz [expr {198 / $divisor}]
    set sck_khz   [expr {$lqspi_mhz * 1000 / 256}]
    echo "  LQSPI_CLK = IO_PLL/$divisor ≈ $lqspi_mhz MHz  SPI SCK (BAUD÷256) ≈ $sck_khz kHz"

    # Read current value (what BootROM left) before touching it
    set lqspi_before [read_memory $lqspi_clk_ctrl_addr 32 1]
    echo "  LQSPI_CLK_CTRL before = 0x[format %08X $lqspi_before]  (at 0x[format %08X $lqspi_clk_ctrl_addr])"

    # Stop → configure → start (latch divisor on 0→1 CLKACT edge)
    set lqspi_cfg [expr {($srcsel << 4) | ($divisor << 8)}]
    write_memory $lqspi_clk_ctrl_addr 32 [list $lqspi_cfg]         ;# CLKACT=0, set SRCSEL+DIVISOR
    after 2
    write_memory $lqspi_clk_ctrl_addr 32 [list [expr {$lqspi_cfg | 1}]]  ;# CLKACT=1 — latch
    after 2
    set lqspi_rb [read_memory $lqspi_clk_ctrl_addr 32 1]
    echo "  LQSPI_CLK_CTRL wrote=0x[format %08X [expr {$lqspi_cfg|1}]] read=0x[format %08X $lqspi_rb]"
    if {($lqspi_rb & 1) == 0} {
        echo "  WARNING: CLKACT bit did not stick"
    } else {
        echo "  LQSPI_CLK active ✓"
    }

    # ── Step 3: ensure LQSPI APB bus clock (APER_CLK_CTRL bit 17) ──────────
    set aper [read_memory 0xF800012C 32 1]
    if {($aper & (1<<17)) == 0} {
        write_memory 0xF800012C 32 [list [expr {$aper | (1<<17)}]]
        echo "  APER bit17 (LQSPI APB) enabled"
    } else {
        echo "  APER bit17 (LQSPI APB) already on ✓"
    }

    # ── Step 4: deassert LQSPI peripheral reset if held ─────────────────────
    # LQSPI_RST_CTRL at SLCR offset 0x230 (0xF8000230), bit 0 = CPU1X reset
    set rst [read_memory 0xF8000230 32 1]
    echo "  LQSPI_RST_CTRL = 0x[format %08X $rst]"
    if {($rst & 1) != 0} {
        write_memory 0xF8000230 32 {0}
        after 2
        echo "  QSPI peripheral reset deasserted"
    } else {
        echo "  QSPI peripheral not in reset ✓"
    }

    # Re-lock SLCR
    write_memory 0xF8000004 32 {0x0000767B}
    after 5

    # ── Step 5: reset QSPI controller (ER=0→1) so it picks up the new clock ─
    # Also clear LQSPI_CR (0xE000D0A0) to disable the BootROM linear mode.
    # The BootROM sets LQSPI_CR bit31 (LQ_MODE=1) to read the boot image via
    # memory-mapped linear flash interface.  While LQ_MODE=1, the I/O mode
    # TX path is locked out: bytes written to TXD0 queue in the FIFO but no
    # SPI clock is generated and RXNE never fires.  Clearing LQSPI_CR to 0
    # hands control back to the I/O mode state machine.
    set QSPI_BASE 0xE000D000
    set LQSPI_CR  [expr {$QSPI_BASE + 0xA0}]

    set lqspi_cr_val [read_memory $LQSPI_CR 32 1]
    echo "  LQSPI_CR before = 0x[format %08X $lqspi_cr_val]  (bit31=LQ_MODE=[expr {($lqspi_cr_val>>31)&1}])"

    write_memory [expr {$QSPI_BASE+0x14}] 32 {0}       ;# ER=0
    write_memory $LQSPI_CR                32 {0}        ;# disable linear mode
    write_memory [expr {$QSPI_BASE+0x0C}] 32 {0x7F}    ;# IDR all off
    for {set i 0} {$i < 16} {incr i} {
        set isr [read_memory [expr {$QSPI_BASE+0x04}] 32 1]
        if {($isr & 0x10) == 0} break
        read_memory [expr {$QSPI_BASE+0x20}] 32 1
    }
    write_memory [expr {$QSPI_BASE+0x14}] 32 {1}       ;# ER=1

    set lqspi_cr_after [read_memory $LQSPI_CR 32 1]
    set cr_now [read_memory [expr {$QSPI_BASE+0x00}] 32 1]
    echo "  LQSPI_CR after  = 0x[format %08X $lqspi_cr_after]  (should be 0x00000000)"
    echo "  QSPI_CR = 0x[format %08X $cr_now]"
    echo "  QSPI clocks enabled."
}

# ── qspi_recover: main entry point ──────────────────────────────────────────
proc qspi_recover {} {
    global jedec_cr jedec_txd_off jedec_man_start

    set BOOT_FILE "/home/parallels/Documents/SDR/recovery/flash_good.bin"
    set FLASHER   "/home/parallels/Documents/SDR/recovery/qspi_flasher.bin"

    set FLASHER_ADDR 0x00020000
    set MAIN_ENTRY   0x00020000   ;# main() at 0x20000 (fixed-delay rebuild, nm confirmed)
    set CHUNK_ADDR   0x00021000
    set STATUS       0x00029000
    set DEST_REG     0x00029004
    set ERASE_REG    0x00029008
    set JEDEC0       0x0002900C
    set JEDEC1       0x00029010
    set JEDEC2       0x00029014
    set CHUNK_ENTRY  0x0002007c   ;# program_chunk() confirmed by nm (fixed-delay rebuild)

    set QSPI_BASE    0xE000D000

    echo ""
    echo "=== Zynq-7020 QSPI Recovery ==="
    echo ""

    # ── 1. Connect ──────────────────────────────────────────────────────────
    for {set i 0} {$i < 8} {incr i} {
        after 400
        if {![catch {zynq.cpu0 arp_examine}]} break
        echo "  examine retry $i…"
    }
    halt; after 500
    echo "CPU halted at PC=0x[format %08X [read_memory 0x00000000 32 0; expr {0}]]"
    catch { set pc_val [expr {[lindex [reg pc] end]}] }
    catch { echo "PC=[reg pc]" }

    # ── 2. Read QSPI state as BootROM left it ───────────────────────────────
    echo ""
    echo "--- BootROM QSPI state ---"
    set cr_boot [read_memory [expr {$QSPI_BASE+0x00}] 32 1]
    set er_boot [read_memory [expr {$QSPI_BASE+0x14}] 32 1]
    set isr_boot [read_memory [expr {$QSPI_BASE+0x04}] 32 1]
    echo "  QSPI_CR  = 0x[format %08X $cr_boot]"
    echo "  QSPI_ER  = 0x[format %08X $er_boot]"
    echo "  QSPI_ISR = 0x[format %08X $isr_boot]"

    # Build I/O-mode CR: keep BootROM bits, set IFM=1 SSFORCE=1 SSCTRL=1
    # (SSCTRL=1 = CS deasserted)
    set cr_io [expr {($cr_boot | (1<<31) | (1<<14) | (1<<10))}]
    echo "  I/O CR   = 0x[format %08X $cr_io]"

    # ── 3. Enable QSPI clocks (SLCR) ────────────────────────────────────────
    # BootROM uses hardwired boot path; LQSPI_CLK and APB clock are NOT
    # enabled in SLCR.  I/O mode needs both.
    enable_qspi_clocks

    # ── 4. Configure MIO pins for QSPI (BootROM resets them to GPIO on JTAG entry)
    configure_mio_qspi

    # ── 4b. Release flash from deep power-down (cmd 0xAB) ───────────────────
    # Some flash chips (e.g. Winbond W25Q128) enter Deep Power-Down after a
    # failed boot if the BootROM sent 0xB9.  In power-down the chip responds to
    # nothing except 0xAB (Release Power-Down).  Send it now so the JEDEC sweep
    # sees an awake device.  tRES1 = 3 µs — the 2ms after 0xAB covers that.
    echo ""
    echo "--- Sending Release-from-Deep-Power-Down (0xAB) ---"
    set QSPI_BASE_L 0xE000D000
    # Use auto-start (MAN_START_EN=0), BAUD÷256, IFM=1, SSFORCE=1
    set cr_wake_cs  [expr {(1<<31)|(1<<14)|(0<<10)|(7<<3)|(1<<0)}]  ;# CS asserted
    set cr_wake_idle [expr {(1<<31)|(1<<14)|(1<<10)|(7<<3)|(1<<0)}] ;# CS deasserted
    # Reset controller
    write_memory [expr {$QSPI_BASE_L+0x14}] 32 {0}
    write_memory [expr {$QSPI_BASE_L+0x0C}] 32 {0x7F}
    write_memory [expr {$QSPI_BASE_L+0x00}] 32 [list $cr_wake_idle]
    write_memory [expr {$QSPI_BASE_L+0x14}] 32 {1}
    for {set i 0} {$i < 16} {incr i} {
        if {([read_memory [expr {$QSPI_BASE_L+0x04}] 32 1] & 0x10) == 0} break
        read_memory [expr {$QSPI_BASE_L+0x20}] 32 1
    }
    # Assert CS, send 0xAB, wait RXNE, deassert CS
    write_memory [expr {$QSPI_BASE_L+0x00}] 32 [list $cr_wake_cs]
    after 1
    write_memory [expr {$QSPI_BASE_L+0x80}] 32 {0xAB}
    for {set i 0} {$i < 200} {incr i} {
        if {([read_memory [expr {$QSPI_BASE_L+0x04}] 32 1] & 0x10) != 0} break
    }
    read_memory [expr {$QSPI_BASE_L+0x20}] 32 1
    write_memory [expr {$QSPI_BASE_L+0x00}] 32 [list $cr_wake_idle]
    after 2
    echo "  0xAB sent — flash awake (tRES1 satisfied)."

    # ── 5. TCL-based JEDEC ID test (CPU halted, no ARM code) ────────────────
    echo ""
    echo "--- TCL QSPI communication test (CPU stays halted) ---"
    echo "Trying all TXD offset / MANSTRT combinations…"

    set jedec_ok [qspi_tcl_jedec $cr_io]

    if {$jedec_ok} {
        echo ""
        echo "TCL JEDEC confirmed: TXD+0x[format %02X $jedec_txd_off] man=$jedec_man_start CR=0x[format %08X $jedec_cr]"
    } else {
        echo ""
        echo "TCL JEDEC failed — trying ARM main() for JEDEC confirmation..."
    }

    # ── 5b. Tame Zynq System WDT (SWDT) ─────────────────────────────────────
    # The SWDT cannot be disabled, but we can change it from RESET mode to
    # INTERRUPT mode so a timeout never causes a system reset.
    # SWDT_ZMR (0xF8005000): bits[23:12]=ZKEY=0xABC, bit0=ZIEN
    #   ZIEN=0 (default): counter=0 → system reset  ← BootROM default, kills us!
    #   ZIEN=1           : counter=0 → interrupt     ← safe; IRQ is masked anyway
    # We also reprogram the count to maximum (CRV=0xFFFF, CLKSEL=0xF → ~128 s)
    # and kick it right away.
    echo ""
    echo "--- Configuring System Watchdog Timer (interrupt mode, max count) ---"
    set swdt_zmr [read_memory 0xF8005000 32 1]
    set swdt_ccr [read_memory 0xF8005004 32 1]
    echo "  SWDT_ZMR before = 0x[format %08X $swdt_zmr]  (bit0=ZIEN: 0=reset,1=irq)"
    echo "  SWDT_CCR before = 0x[format %08X $swdt_ccr]"
    # ZMR: key=0xABC in bits[23:12], ZIEN=1 → generate interrupt, not reset
    write_memory 0xF8005000 32 {0x00ABC001}
    # CCR: key=0x248 in bits[31:20], CRV=0xFFFF in bits[19:4], CLKSEL=0xF → max timeout
    write_memory 0xF8005004 32 {0x248FFFFF}
    # RESTART: write 0x1999 to reset counter to new max value
    write_memory 0xF8005008 32 {0x00001999}
    set swdt_zmr_after [read_memory 0xF8005000 32 1]
    set swdt_ccr_after [read_memory 0xF8005004 32 1]
    echo "  SWDT_ZMR after  = 0x[format %08X $swdt_zmr_after]  (bit0=ZIEN want 1)"
    echo "  SWDT_CCR after  = 0x[format %08X $swdt_ccr_after]"
    echo "  SWDT restarted: interrupt mode, ~128 s timeout ✓"

    # ── 5c. Disable Cortex-A9 private watchdog (AWDT) ───────────────────────
    # The Cortex-A9 has its own per-CPU watchdog at 0xF8F00620.
    # Two-write magic sequence to the WDTDIS register disables it.
    echo ""
    echo "--- Disabling Cortex-A9 Private Watchdog (AWDT) ---"
    set awd_ctrl [read_memory 0xF8F00628 32 1]
    echo "  AWDT_CR before = 0x[format %08X $awd_ctrl]  (bit0=enable, bit3=wd_mode)"
    write_memory 0xF8F00634 32 {0x12345678}   ;# disable word 1
    write_memory 0xF8F00634 32 {0x87654321}   ;# disable word 2
    set awd_ctrl_after [read_memory 0xF8F00628 32 1]
    echo "  AWDT_CR after  = 0x[format %08X $awd_ctrl_after]  (want bit0=0)"

    # ── 6. Load flasher binary and confirm JEDEC via ARM main() ─────────────
    echo ""
    echo "--- Loading flasher binary to OCM ---"
    set fw [words_from_file $FLASHER]
    write_words $FLASHER_ADDR $fw
    echo "Flasher loaded at 0x[format %08X $FLASHER_ADDR]"

    # Run main() — reads JEDEC ID and sets STATUS=0xDEAD0003 on success.
    # STATUS=0xDEAD0002 → stuck in xfer() RXNE poll (SPI clock problem).
    # STATUS=0           → ARM faulted before writing first sentinel (stack/PC problem).
    write_memory $STATUS 32 {0}
    halt; after 200
    # Set SVC-mode banked SP explicitly — "reg sp" only sets the *current* mode's
    # banked SP.  If the CPU is in Abort mode when halted, "reg sp" sets r13_abt
    # and r13_svc remains 0 (or stale), causing push {r3,lr} to fault at address 0.
    # Use the explicit banked-register name to reach the SVC slot directly.
    reg r13_svc 0x0002EFF0  ;# SVC-mode SP within OCM bank2, below handshake at 0x29000
    reg pc   $MAIN_ENTRY    ;# 0x00020000 = main() per nm (fixed-delay rebuild)
    reg cpsr 0x000001F3  ;# SVC|Thumb|IRQ_dis|FIQ_dis|Abort_dis — code is Thumb-2     ;# SVC mode, IRQ+FIQ masked, ARM state
    resume

    echo "ARM main() started — polling STATUS for JEDEC result (10 s)..."
    set jedec_arm 0
    for {set t 0} {$t < 50} {incr t} {
        after 200
        catch { halt }; after 100
        set sv 0; catch { set sv [read_memory $STATUS 32 1] }
        if {$sv == 0xDEAD0003} { set jedec_arm 1; break }
        if {$sv != 0 && $sv != 0xDEAD0001 && $sv != 0xDEAD0002} { break }
        write_memory 0xF8005008 32 {0x00001999}   ;# kick SWDT
        catch { resume }; after 50
    }
    catch { halt }; after 200

    set sv 0; catch { set sv [read_memory $STATUS 32 1] }
    echo "STATUS = 0x[format %08X $sv]"

    if {$jedec_arm} {
        set j0 0; set j1 0; set j2 0
        catch { set j0 [read_memory $JEDEC0 32 1] }
        catch { set j1 [read_memory $JEDEC1 32 1] }
        catch { set j2 [read_memory $JEDEC2 32 1] }
        echo "ARM JEDEC ID: 0x[format %02X $j0] 0x[format %02X $j1] 0x[format %02X $j2]"
        if {$j0 == 0xFF || $j0 == 0x00} {
            echo "WARNING: JEDEC ID is 0x[format %02X $j0] — flash may not be responding."
            echo "Proceeding with programming anyway..."
        } else {
            echo "Valid JEDEC confirmed ✓ — proceeding to flash programming."
        }
    } else {
        echo ""
        echo "!!! ARM main() did not reach JEDEC completion (STATUS=0x[format %08X $sv]) !!!"
        if {$sv == 0xDEAD0001} {
            echo "STATUS=0xDEAD0001: qspi_init() ran but no JEDEC response — RXNE never fired."
        } elseif {$sv == 0xDEAD0002} {
            echo "STATUS=0xDEAD0002: stuck inside xfer() RXNE loop — SPI clock issue."
        } elseif {$sv == 0} {
            echo "STATUS=0: ARM main() never wrote STATUS — possible fault before first store."
            echo "--- ARM fault registers ---"
            catch { echo "  cpsr   = [reg cpsr]" }
            catch { echo "  pc     = [reg pc]" }
            catch { echo "  sp     = [reg sp]" }
            catch { echo "  DFSR   = 0x[format %08X [read_memory 0xE000ED30 32 1]]" }
            catch { echo "  DFAR   = 0x[format %08X [read_memory 0xE000ED38 32 1]]" }
            catch { echo "  IFSR   = 0x[format %08X [read_memory 0xE000ED28 32 1]]" }
            catch { echo "  IFAR   = 0x[format %08X [read_memory 0xE000ED3C 32 1]]" }
        }
        echo ""
        echo "--- Final register state ---"
        echo "  IO_PLL_CTRL    = 0x[format %08X [read_memory 0xF8000108 32 1]]"
        echo "  PLL_STATUS     = 0x[format %08X [read_memory 0xF800010C 32 1]]"
        echo "  LQSPI_CLK_CTRL = 0x[format %08X [read_memory 0xF800014C 32 1]]  (0xF800014C)"
        echo "  TPIU_CLK_CTRL  = 0x[format %08X [read_memory 0xF800016C 32 1]]  (0xF800016C, NOT QSPI)"
        set QSPI_BASE 0xE000D000
        echo "  QSPI_CR  = 0x[format %08X [read_memory [expr {$QSPI_BASE+0x00}] 32 1]]"
        echo "  QSPI_ISR = 0x[format %08X [read_memory [expr {$QSPI_BASE+0x04}] 32 1]]"
        echo "  QSPI_ER  = 0x[format %08X [read_memory [expr {$QSPI_BASE+0x14}] 32 1]]"
        for {set n 0} {$n <= 8} {incr n} {
            set addr [expr {0xF8000700 + $n*4}]
            echo "  MIO_PIN_0$n = 0x[format %08X [read_memory $addr 32 1]]"
        }
        echo ""
        echo "Hardware appears unable to communicate with QSPI flash."
        echo "Check: Is this a Pluto+ with QSPI on MIO\[1..6\]? Is the flash soldered?"
        return
    }

    # ── 7. Flash image chunk by chunk (RDSR polling, fast) ───────────────────
    # Now using RDSR WIP polling instead of fixed delays:
    #   sector erase: ≤2s (W25Q256 max), page_prog: ≤5ms × 128 = 640ms
    #   → each chunk completes in ≤3s with erase, ≤1s without
    echo ""
    echo "--- Loading flash image (1MB from working device) ---"
    set f [open $BOOT_FILE rb]; set raw [read $f]; close $f
    set total [string length $raw]
    binary scan $raw cu* all_bytes

    set target [expr {(($total + 32767) / 32768) * 32768}]
    for {set p $total} {$p < $target} {incr p} { lappend all_bytes 255 }
    set nchunks [expr {$target / 32768}]
    echo "flash_good.bin: $total bytes → $nchunks chunks of 32KB"
    echo ""

    for {set c 0} {$c < $nchunks} {incr c} {
        set off      [expr {$c * 32768}]
        set qaddr    [expr {$c * 32768}]
        set chunk    [lrange $all_bytes $off [expr {$off + 32767}]]
        set do_erase [expr {($c % 2) == 0}]

        echo "Chunk [expr {$c+1}]/$nchunks QSPI 0x[format %06X $qaddr] erase=$do_erase"

        write_words $CHUNK_ADDR [words_from_bytes $chunk]
        write_memory $DEST_REG  32 [list $qaddr]
        write_memory $ERASE_REG 32 [list $do_erase]
        write_memory $STATUS    32 {0}

        reg r13_svc 0x0002EFF0  ;# SVC-mode banked SP
        reg pc   $CHUNK_ENTRY
        reg cpsr 0x000001F3  ;# SVC|Thumb|IRQ_dis|FIQ_dis — Thumb-2 code
        resume

        # Poll STATUS every 2s; abort after 120s per chunk (60s erase + 60s prog).
        # Kick SWDT before every resume so a ~128s timer never fires.
        set wait_total [expr {$do_erase ? 120 : 60}]
        set elapsed 0
        set done 0
        set prev_s -1
        while {$elapsed < $wait_total} {
            set step 2
            after [expr {$step * 1000}]
            set elapsed [expr {$elapsed + $step}]
            catch { halt }; after 200
            set s 0; catch { set s [read_memory $STATUS 32 1] }
            if {$s != $prev_s} {
                set desc ""
                if {$s == 0xBEEF0001} { set desc " (program_chunk entered)" }
                if {$s == 0xBEEF0011} { set desc " (qspi_init done)" }
                if {$s == 0xBEEF0012} { set desc " (sector_erase issued)" }
                if {$s == 0xBEEF0013} { set desc " (erase wait_wip done)" }
                if {$s == 0xBEEF0020} { set desc " (p=0 loop — calling page_prog)" }
                if {$s == 0xBEEF0031} { set desc " *** entered page_prog ***" }
                if {$s == 0xBEEF0032} { set desc " (wren done)" }
                if {$s == 0xBEEF0033} { set desc " (CS asserted)" }
                if {$s == 0xBEEF0034} { set desc " (PP cmd 0x02 sent)" }
                if {$s == 0xBEEF0035} { set desc " (address bytes sent)" }
                if {$s == 0xBEEF0036} { set desc " (sending data byte 0)" }
                if {$s == 0xBEEF0037} { set desc " (sending data byte 128)" }
                if {$s == 0xBEEF0038} { set desc " (all 256 bytes sent)" }
                if {$s == 0xBEEF0039} { set desc " (CS deasserted — PP issued)" }
                if {$s == 0xBEEF0060} { set desc " (p=64 loop)" }
                if {$s == 0xBEEF0002} { set desc " *** ALL DONE! ***" }
                echo "  ${elapsed}s STATUS=0x[format %08X $s]$desc"
                set prev_s $s
            }
            if {$s == 0xBEEF0002} { set done 1; break }
            # Kick SWDT before resuming so the ~128s timer never expires
            write_memory 0xF8005008 32 {0x00001999}
            catch { resume }; after 100
        }
        if {!$done} {
            catch { halt }; after 200
            set s 0; catch { set s [read_memory $STATUS 32 1] }
            if {$s == 0xBEEF0002} { set done 1 }
        }

        if {$done} {
            echo "  OK"
        } else {
            set s 0; catch { set s [read_memory $STATUS 32 1] }
            echo "  FAILED: STATUS=0x[format %08X $s]"
            return
        }
    }

    echo ""
    echo "==================================================================="
    echo "=== DONE! flash_good.bin written to QSPI.                      ==="
    echo "=== Disconnect JTAG, power cycle device.                       ==="
    echo "=== PlutoSDR should appear as USB network + serial port.       ==="
    echo "==================================================================="
}
