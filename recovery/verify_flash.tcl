proc verify_flash {} {
    set QSPI_BASE  0xE000D000
    set LQSPI_CR   [expr {$QSPI_BASE + 0xA0}]
    set LQSPI_ADDR 0xFC000000

    for {set i 0} {$i < 8} {incr i} {
        after 400
        if {![catch {zynq.cpu0 arp_examine}]} break
    }
    halt; after 500
    echo "--- Flash verification ---"

    # 1. SLCR unlock
    write_memory 0xF8000008 32 {0x0000DF0D}
    after 2

    # 2. IO PLL to 198 MHz (safe known state)
    set io_pll [read_memory 0xF8000108 32 1]
    set pll_r   [expr {($io_pll & ~(0x7F<<12)) | (1<<3) | (6<<12)}]
    write_memory 0xF8000108 32 [list $pll_r]
    after 2
    write_memory 0xF8000108 32 [list [expr {$pll_r & ~0x8}]]
    for {set t 0} {$t < 100} {incr t} {
        after 2
        if {([read_memory 0xF800010C 32 1] & 0x4) != 0} break
    }
    echo "  PLL_STATUS = 0x[format %08X [read_memory 0xF800010C 32 1]]"

    # 3. LQSPI_CLK = IO_PLL/5 ~ 40 MHz
    write_memory 0xF800014C 32 {0x00000500}
    after 2
    write_memory 0xF800014C 32 {0x00000501}
    after 2
    echo "  LQSPI_CLK_CTRL = 0x[format %08X [read_memory 0xF800014C 32 1]]"

    # 4. APER APB clock (bit17)
    set aper [read_memory 0xF800012C 32 1]
    if {($aper & (1<<17)) == 0} {
        write_memory 0xF800012C 32 [list [expr {$aper | (1<<17)}]]
    }

    # 5. Deassert LQSPI reset
    set rst [read_memory 0xF8000230 32 1]
    if {($rst & 1) != 0} { write_memory 0xF8000230 32 {0} ; after 2 }

    # 6. MIO pins for QSPI (BootROM resets these on JTAG entry)
    set MIO_BASE 0xF8000700
    set vals {0x1200 0x1202 0x0202 0x0202 0x0202 0x0202 0x0202}
    for {set i 0} {$i < 7} {incr i} {
        set addr [expr {$MIO_BASE + $i*4}]
        set old  [read_memory $addr 32 1]
        set v    [lindex $vals $i]
        set new  [expr {($old & ~0x3FFF) | ($v & 0x3FFF)}]
        write_memory $addr 32 [list $new]
    }
    echo "  MIO_01 = 0x[format %08X [read_memory [expr {$MIO_BASE+4}] 32 1]] (QSPI SS_B)"

    # 7. SLCR re-lock
    write_memory 0xF8000004 32 {0x0000767B}
    after 5

    # 8. Configure QSPI controller in linear mode
    write_memory [expr {$QSPI_BASE+0x14}] 32 {0}           ;# ER=0
    write_memory [expr {$QSPI_BASE+0x0A0}] 32 {0x80000003} ;# LQSPI_CR: LQ_MODE=1
    write_memory [expr {$QSPI_BASE+0x00}]  32 {0x80004439} ;# CR_IDLE
    write_memory [expr {$QSPI_BASE+0x14}] 32 {1}           ;# ER=1
    after 10

    echo "  LQSPI_CR = 0x[format %08X [read_memory $LQSPI_CR 32 1]]"
    echo "  QSPI_CR  = 0x[format %08X [read_memory $QSPI_BASE 32 1]]"
    echo ""
    echo "  Linear read from 0xFC000000 (first 16 words):"

    set words {}
    for {set i 0} {$i < 16} {incr i} {
        set v 0
        catch { set v [read_memory [expr {$LQSPI_ADDR + $i*4}] 32 1] }
        lappend words $v
        echo "    \[[format %02d $i]\] 0x[format %08X $v]"
    }

    # Evaluate
    set w0 [lindex $words 0]
    if {$w0 == 0xEAFFFFFE} {
        echo ""
        echo "PASS — boot.dfu header found at 0xFC000000 — device should boot!"
    } elseif {$w0 == 0xFFFFFFFF} {
        echo ""
        echo "FAIL — flash reads 0xFFFFFFFF (erased, not programmed)."
        echo "The page_program commands were ignored (erase not complete before write)."
        echo "Need to re-flash with wait_ready() replaced by fixed delays."
    } elseif {$w0 == 0x00000000} {
        echo ""
        echo "FAIL — reads all zeros (MISO stuck 0 or LQSPI not working)."
        echo "Flash state unknown."
    } else {
        echo ""
        echo "FAIL — unexpected data (0x[format %08X $w0]) — flash partially corrupted."
    }
}
