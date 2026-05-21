
# Pure-TCL page_prog smoke test
# Writes 256 bytes of 0xA5 to flash address 0x000000 (must be erased = 0xFF first)
# then reads back via LQSPI linear mode to verify.
# Usage: openocd -f zynq7020_ft4232.cfg -c "source test_page_prog.tcl; init; after 500; test_pp; shutdown"

proc tcl_xfer {byte} {
    set BASE 0xE000D000
    # wait TX not full (ISR bit 3)
    for {set i 0} {$i < 2000} {incr i} {
        if {([read_memory [expr {$BASE+0x04}] 32 1] & 0x08) == 0} break
    }
    write_memory [expr {$BASE+0x80}] 32 [list [expr {$byte & 0xFF}]]
    # wait RXNE (ISR bit 4); count iterations to detect if transfer fires
    set rxne_iters 0
    for {set i 0} {$i < 2000} {incr i} {
        incr rxne_iters
        if {([read_memory [expr {$BASE+0x04}] 32 1] & 0x10) != 0} break
    }
    set rxd [read_memory [expr {$BASE+0x20}] 32 1]
    return [list [expr {($rxd >> 24) & 0xFF}] $rxne_iters]
}

proc tcl_xfer1 {byte} {
    set r [tcl_xfer $byte]
    return [lindex $r 0]
}

proc tcl_xfer_diag {label byte} {
    set BASE 0xE000D000
    set r [tcl_xfer $byte]
    set rx  [lindex $r 0]
    set its [lindex $r 1]
    set isr [read_memory [expr {$BASE+0x04}] 32 1]
    echo "  $label: tx=0x[format %02X $byte] rx=0x[format %02X $rx] rxne_iters=$its ISR=0x[format %08X $isr]"
    return $rx
}

proc test_pp {} {
    set BASE  0xE000D000
    set LQSPI_CR_ADDR [expr {$BASE + 0xA0}]
    set CR_IDLE 0x80004439
    set CR_CS   0x80004039

    echo "=== TCL Page-Program Smoke Test ==="
    halt; after 300

    # 1. Setup clocks + MIO (same as full recovery)
    write_memory 0xF8000008 32 {0x0000DF0D}
    set pll [read_memory 0xF8000108 32 1]
    set pr  [expr {($pll & ~(0x7F<<12)) | (1<<3) | (6<<12)}]
    write_memory 0xF8000108 32 [list $pr]
    after 2
    write_memory 0xF8000108 32 [list [expr {$pr & ~8}]]
    for {set i 0} {$i < 100} {incr i} { after 2; if {([read_memory 0xF800010C 32 1] & 4) != 0} break }
    write_memory 0xF800014C 32 {0x00000500}; after 2
    write_memory 0xF800014C 32 {0x00000501}; after 2
    set v [read_memory 0xF800012C 32 1]
    write_memory 0xF800012C 32 [list [expr {$v | (1<<17)}]]

    # MIO pins
    set MB 0xF8000700
    foreach {i val} {0 0x1200 1 0x1202 2 0x0202 3 0x0202 4 0x0202 5 0x0202 6 0x0202} {
        set a [expr {$MB + $i*4}]
        set o [read_memory $a 32 1]
        write_memory $a 32 [list [expr {($o & ~0x3FFF) | ($val & 0x3FFF)}]]
    }
    write_memory 0xF8000004 32 {0x0000767B}

    # Init QSPI controller
    write_memory [expr {$BASE+0x14}] 32 {0}
    write_memory $LQSPI_CR_ADDR      32 {0}
    write_memory [expr {$BASE+0x0C}] 32 {0x7F}
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    write_memory [expr {$BASE+0x14}] 32 {1}
    for {set i 0} {$i < 16} {incr i} {
        if {([read_memory [expr {$BASE+0x04}] 32 1] & 0x10) == 0} break
        read_memory [expr {$BASE+0x20}] 32 1
    }
    echo "QSPI init done. CR=0x[format %08X [read_memory [expr {$BASE+0x00}] 32 1]]"
    echo "ISR after init: 0x[format %08X [read_memory [expr {$BASE+0x04}] 32 1]]"

    # Diagnostic: read JEDEC ID via TXD1 to confirm transfers fire
    echo ""
    echo "--- JEDEC ID probe (TXD1 diagnostic) ---"
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    set j0 [tcl_xfer_diag "CMD_9F"   0x9F]
    set j1 [tcl_xfer_diag "JEDEC_b0" 0xFF]
    set j2 [tcl_xfer_diag "JEDEC_b1" 0xFF]
    set j3 [tcl_xfer_diag "JEDEC_b2" 0xFF]
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    echo "JEDEC ID: 0x[format %02X $j1] 0x[format %02X $j2] 0x[format %02X $j3]"
    if {$j1 == 0xFF || ($j1 == 0x00 && $j2 == 0x00 && $j3 == 0x00)} {
        echo "WARNING: JEDEC ID suspicious — TXD1 may not be triggering transfers"
        echo "         (rxne_iters shown above — if always 2000 = transfer not firing)"
    } else {
        echo "JEDEC ID looks valid — TXD1 is working"
    }

    # 2. Sector erase 0x000000 (to ensure clean slate)
    echo ""
    echo "Erasing sector 0x000000..."
    # WREN
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    tcl_xfer_diag "WREN" 0x06
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    after 1
    # Read RDSR — check WEL bit (bit 1)
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    tcl_xfer_diag "RDSR_cmd" 0x05
    set sr [tcl_xfer_diag "RDSR_data" 0xFF]
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    echo "  Status Register after WREN: 0x[format %02X $sr]  WEL=[expr {($sr>>1)&1}]  WIP=[expr {$sr&1}]"
    # D8h erase
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    tcl_xfer1 0xD8; tcl_xfer1 0x00; tcl_xfer1 0x00; tcl_xfer1 0x00
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    echo "Erase command sent. Waiting 5 seconds..."
    after 5000

    # Re-init controller after wait
    write_memory [expr {$BASE+0x14}] 32 {0}
    write_memory $LQSPI_CR_ADDR      32 {0}
    write_memory [expr {$BASE+0x0C}] 32 {0x7F}
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    write_memory [expr {$BASE+0x14}] 32 {1}
    for {set i 0} {$i < 16} {incr i} {
        if {([read_memory [expr {$BASE+0x04}] 32 1] & 0x10) == 0} break
        read_memory [expr {$BASE+0x20}] 32 1
    }
    echo "Controller re-inited after erase wait."

    # 3. Page program 256 bytes of 0xA5 to address 0x000000
    echo ""
    echo "Programming 256 bytes of 0xA5 to 0x000000..."
    # WREN
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    tcl_xfer_diag "WREN" 0x06
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    after 1
    # Read RDSR — check WEL bit
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    tcl_xfer1 0x05
    set sr [tcl_xfer_diag "RDSR" 0xFF]
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    echo "  Status Register after WREN: 0x[format %02X $sr]  WEL=[expr {($sr>>1)&1}]  WIP=[expr {$sr&1}]"
    # Page prog
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    tcl_xfer1 0x02; tcl_xfer1 0x00; tcl_xfer1 0x00; tcl_xfer1 0x00
    for {set i 0} {$i < 256} {incr i} { tcl_xfer1 0xA5 }
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    echo "Page prog command sent. Waiting 100ms..."
    after 100

    # Re-init for linear read
    write_memory [expr {$BASE+0x14}] 32 {0}
    write_memory $LQSPI_CR_ADDR      32 {0x80000003}
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    write_memory [expr {$BASE+0x14}] 32 {1}
    after 10

    # 4. Read back via linear mode
    echo ""
    echo "Reading back 0xFC000000 (flash addr 0x000000):"
    set pass 1
    for {set i 0} {$i < 8} {incr i} {
        set v 0
        catch { set v [read_memory [expr {0xFC000000 + $i*4}] 32 1] }
        echo "  \[[format %02d $i]\] 0x[format %08X $v]"
        if {$v != 0xA5A5A5A5} { set pass 0 }
    }
    echo ""
    if {$pass} {
        echo "PASS — page_prog works! Flash wrote 0xA5A5A5A5 correctly."
    } else {
        set w0 0; catch { set w0 [read_memory 0xFC000000 32 1] }
        if {$w0 == 0xFFFFFFFF} {
            echo "FAIL — still 0xFF. Page_prog command not reaching flash."
            echo "Possible causes: WP# asserted, status register protection, or controller issue."
        } else {
            echo "PARTIAL — unexpected data. Flash may be responding differently."
        }
    }
}
