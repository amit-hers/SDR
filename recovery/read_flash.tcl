
# read_flash.tcl — Dump working Pluto+ QSPI flash to a binary file via LQSPI linear mode.
# Usage: openocd -f zynq7020_ft4232.cfg -c "source read_flash.tcl; init; after 500; read_flash; shutdown"
#
# Reads flash from 0xFC000000 (LQSPI AXI window) in 4KB blocks.
# Default: first 1MB (boot partition header area). Change SIZE_KB to read more.

proc read_flash {{outfile "flash_dump.bin"} {size_kb 1024}} {
    set BASE 0xE000D000
    set LQSPI_CR_ADDR [expr {$BASE + 0xA0}]
    set CR_IDLE 0x80004439

    echo ""
    echo "=== QSPI Flash Read (working device) ==="
    halt; after 300

    # Setup SLCR + MIO (same as recovery)
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

    set MB 0xF8000700
    foreach {i val} {0 0x1200 1 0x1202 2 0x0202 3 0x0202 4 0x0202 5 0x0202 6 0x0202} {
        set a [expr {$MB + $i*4}]
        set o [read_memory $a 32 1]
        write_memory $a 32 [list [expr {($o & ~0x3FFF) | ($val & 0x3FFF)}]]
    }
    write_memory 0xF8000004 32 {0x0000767B}

    # Enable LQSPI linear mode (0x03 READ command)
    write_memory [expr {$BASE+0x14}] 32 {0}
    write_memory $LQSPI_CR_ADDR      32 {0x80000003}
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    write_memory [expr {$BASE+0x14}] 32 {1}
    after 10

    # Verify JEDEC via I/O mode first
    write_memory [expr {$BASE+0x14}] 32 {0}
    write_memory $LQSPI_CR_ADDR      32 {0}
    write_memory [expr {$BASE+0x0C}] 32 {0x7F}
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    write_memory [expr {$BASE+0x14}] 32 {1}
    set CR_CS 0x80004039
    write_memory [expr {$BASE+0x00}] 32 [list $CR_CS]
    write_memory [expr {$BASE+0x80}] 32 {0x9F}
    after 1
    for {set i 0} {$i < 200} {incr i} { if {([read_memory [expr {$BASE+0x04}] 32 1] & 0x10) != 0} break }
    read_memory [expr {$BASE+0x20}] 32 1
    foreach cmd {0xFF 0xFF 0xFF} {
        write_memory [expr {$BASE+0x80}] 32 [list [expr {$cmd & 0xFF}]]
        for {set i 0} {$i < 200} {incr i} { if {([read_memory [expr {$BASE+0x04}] 32 1] & 0x10) != 0} break }
    }
    set j1 [expr {([read_memory [expr {$BASE+0x20}] 32 1] >> 24) & 0xFF}]
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    echo "JEDEC MFR: 0x[format %02X $j1]  (0xEF=Winbond, 0xC2=Macronix, 0x20=Micron)"
    if {$j1 == 0xFF || $j1 == 0x00} {
        echo "ERROR: No flash detected. Check connections."
        return
    }

    # Re-enable LQSPI linear mode for readback
    write_memory [expr {$BASE+0x14}] 32 {0}
    write_memory $LQSPI_CR_ADDR      32 {0x80000003}
    write_memory [expr {$BASE+0x00}] 32 [list $CR_IDLE]
    write_memory [expr {$BASE+0x14}] 32 {1}
    after 10

    # Read flash in 4KB blocks (1024 words = 4096 bytes each)
    set size_bytes [expr {$size_kb * 1024}]
    set n_words    [expr {$size_bytes / 4}]
    set LQSPI_BASE 0xFC000000

    echo "Reading ${size_kb}KB from flash (${size_bytes} bytes)..."
    set all_data {}

    set block_words 1024  ;# 4KB per block
    set n_blocks [expr {$n_words / $block_words}]

    for {set blk 0} {$blk < $n_blocks} {incr blk} {
        set addr [expr {$LQSPI_BASE + $blk * $block_words * 4}]
        set words [read_memory $addr 32 $block_words]
        foreach w $words {
            # Little-endian word → 4 bytes
            lappend all_data [expr {$w & 0xFF}]
            lappend all_data [expr {($w >> 8) & 0xFF}]
            lappend all_data [expr {($w >> 16) & 0xFF}]
            lappend all_data [expr {($w >> 24) & 0xFF}]
        }
        if {($blk % 16) == 0} {
            echo "  [expr {$blk * 4}]KB / ${size_kb}KB..."
        }
    }

    # Write binary file
    set f [open $outfile wb]
    foreach byte $all_data {
        puts -nonewline $f [binary format c $byte]
    }
    close $f

    echo "Done. Wrote [llength $all_data] bytes to $outfile"
    echo ""
    # Print first 64 bytes as hex
    echo "First 64 bytes of flash:"
    for {set i 0} {$i < 16} {incr i} {
        set row ""
        for {set j 0} {$j < 4} {incr j} {
            set idx [expr {$i*4+$j}]
            append row "[format %02X [lindex $all_data $idx]] "
        }
        echo "  [format %08X [expr {$i*16}]]: $row"
    }
    echo ""
    # Check for Zynq boot header magic
    set w0 [expr {[lindex $all_data 0] | ([lindex $all_data 1]<<8) | ([lindex $all_data 2]<<16) | ([lindex $all_data 3]<<24)}]
    echo "Word0 = 0x[format %08X $w0]"
    if {$w0 == 0xEAFFFFFE} {
        echo "✓ Valid Zynq boot header (ARM branch instruction at offset 0)"
    } elseif {$w0 == 0xFFFFFFFF} {
        echo "✗ Flash appears erased (0xFF)"
    } else {
        echo "? Unknown header — may be non-standard boot format"
    }
}
