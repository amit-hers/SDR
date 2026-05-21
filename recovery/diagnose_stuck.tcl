
# diagnose_stuck.tcl — minimal diagnostic: load flasher, run program_chunk,
# wait for BEEF0020 stuck state, then read QSPI hardware registers.

proc diagnose_chunk {} {
    set FLASHER      "/home/parallels/Documents/SDR/recovery/qspi_flasher.bin"
    set FLASHER_ADDR 0x00020000
    set CHUNK_ADDR   0x00021000
    set STATUS       0x00029000
    set DEST_REG     0x00029004
    set ERASE_REG    0x00029008
    set CHUNK_ENTRY  0x0002007C
    set QSPI_BASE    0xE000D000

    halt; after 300

    # SLCR + MIO setup
    write_memory 0xF8000008 32 {0x0000DF0D}
    set pll [read_memory 0xF8000108 32 1]
    set pr  [expr {($pll & ~(0x7F<<12)) | (1<<3) | (6<<12)}]
    write_memory 0xF8000108 32 [list $pr]; after 2
    write_memory 0xF8000108 32 [list [expr {$pr & ~8}]]
    for {set i 0} {$i < 100} {incr i} { after 2; if {([read_memory 0xF800010C 32 1] & 4) != 0} break }
    write_memory 0xF800014C 32 {0x00000500}; after 2
    write_memory 0xF800014C 32 {0x00000501}; after 2
    set v [read_memory 0xF800012C 32 1]
    write_memory 0xF800012C 32 [list [expr {$v | (1<<17)}]]
    set MB 0xF8000700
    foreach {i val} {0 0x1200 1 0x1202 2 0x0202 3 0x0202 4 0x0202 5 0x0202 6 0x0202} {
        set a [expr {$MB + $i*4}]; set o [read_memory $a 32 1]
        write_memory $a 32 [list [expr {($o & ~0x3FFF) | ($val & 0x3FFF)}]]
    }
    write_memory 0xF8000004 32 {0x0000767B}

    # Load flasher binary word-by-word
    set fh [open $FLASHER rb]
    set fb [read $fh]
    close $fh
    set nbytes [string length $fb]
    set addr $FLASHER_ADDR
    for {set i 0} {$i < $nbytes} {incr i 4} {
        set b0 [expr {[scan [string index $fb $i] %c] & 0xFF}]
        set b1 0; set b2 0; set b3 0
        if {$i+1 < $nbytes} {set b1 [expr {[scan [string index $fb [expr {$i+1}]] %c] & 0xFF}]}
        if {$i+2 < $nbytes} {set b2 [expr {[scan [string index $fb [expr {$i+2}]] %c] & 0xFF}]}
        if {$i+3 < $nbytes} {set b3 [expr {[scan [string index $fb [expr {$i+3}]] %c] & 0xFF}]}
        set w [expr {$b0 | ($b1<<8) | ($b2<<16) | ($b3<<24)}]
        write_memory $addr 32 [list $w]
        incr addr 4
    }
    echo "Flasher loaded ([expr {$nbytes}] bytes)"

    # Fill chunk buffer with 0xA5A5A5A5 pattern (32KB / 4 = 8192 words)
    set pat [list]
    for {set i 0} {$i < 64} {incr i} { lappend pat 0xA5A5A5A5 }
    set caddr $CHUNK_ADDR
    for {set blk 0} {$blk < 128} {incr blk} {
        write_memory $caddr 32 $pat
        incr caddr 256
    }
    echo "Chunk buffer filled with 0xA5 pattern"

    write_memory $DEST_REG  32 {0x000000}
    write_memory $ERASE_REG 32 {1}
    write_memory $STATUS    32 {0}

    reg r13_svc 0x0002EFF0
    reg pc      $CHUNK_ENTRY
    reg cpsr    0x000001F3
    resume
    echo "ARM program_chunk started. Polling every 10s..."

    set prev_s -1
    for {set t 0} {$t <= 250} {incr t 10} {
        after 10000
        catch { halt }; after 200
        set s 0; catch { set s [read_memory $STATUS 32 1] }
        if {$s != $prev_s} {
            echo "  ${t}s STATUS=0x[format %08X $s]"
            set prev_s $s
        }
        if {$s == 0xBEEF0002} { echo "DONE!"; return }
        if {$s == 0xBEEF0020 || $s == 0xBEEF0060} {
            # Stuck in page prog — read QSPI state NOW (CPU already halted)
            echo ""
            echo "=== ARM stuck at STATUS=0x[format %08X $s] — reading QSPI hardware ==="
            set ISR [read_memory [expr {$QSPI_BASE+0x04}] 32 1]
            set CR  [read_memory [expr {$QSPI_BASE+0x00}] 32 1]
            set ER  [read_memory [expr {$QSPI_BASE+0x14}] 32 1]
            echo "  QSPI_CR  = 0x[format %08X $CR]"
            echo "  QSPI_ER  = 0x[format %08X $ER]"
            echo "  QSPI_ISR = 0x[format %08X $ISR]"
            echo "    bit2 TXNFULL=[expr {($ISR>>2)&1}]  bit3 TXFULL=[expr {($ISR>>3)&1}]  bit4 RXNE=[expr {($ISR>>4)&1}]"
            set pc_v  0; catch { set pc_v  [read_register pc] }
            set sp_v  0; catch { set sp_v  [read_register sp] }
            set lr_v  0; catch { set lr_v  [read_register lr] }
            set cps_v 0; catch { set cps_v [read_register cpsr] }
            echo "  CPU PC=0x[format %08X $pc_v]  SP=0x[format %08X $sp_v]  LR=0x[format %08X $lr_v]"
            echo "  CPSR=0x[format %08X $cps_v]  mode=[expr {$cps_v & 0x1F}]  T=[expr {($cps_v>>5)&1}]"
            echo ""
            if {($ISR & 0x08) != 0} {
                echo "  >>> TXFULL=1: ARM is stuck in 'while(ISR & TXFULL)' — TX FIFO stuck full <<<"
            } elseif {($ISR & 0x10) == 0} {
                echo "  >>> RXNE=0, TXFULL=0: ARM wrote to TXD1 but transfer never completed <<<"
                echo "  >>> This means TXD1 write did NOT trigger an SPI transfer              <<<"
            } else {
                echo "  >>> RXNE=1: transfer happened but ARM didn't read RXD yet (unexpected) <<<"
            }
            return
        }
        catch { resume }; after 100
    }
    echo "Timed out"
}
