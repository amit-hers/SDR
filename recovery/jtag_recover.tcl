# Zynq-7020 JTAG Recovery via FSBL load
# Load FSBL to OCM at 0xFFFC0000, execute it, then use DFU to reflash

proc recover {} {
    # Step 1: halt CPU
    halt
    after 200

    set pc [ocd_reg pc]
    echo "CPU halted. PC = $pc"

    # Step 2: Disable MMU and D-cache for reliable memory access
    # Read SCTLR
    set sctlr [arm mrc 15 0 1 0 0]
    echo "SCTLR: [format 0x%08X $sctlr]"
    # Clear M(0), C(2), Z(11), I(12) bits  
    set new_sctlr [expr {$sctlr & ~0x1805}]
    arm mcr 15 0 1 0 0 $new_sctlr
    echo "MMU/Cache disabled"

    # Step 3: Load FSBL to OCM at 0xFFFC0000
    echo "Loading FSBL to OCM at 0xFFFC0000..."
    load_image /home/parallels/Documents/SDR/recovery/fsbl.bin 0xFFFC0000 bin
    echo "FSBL loaded."

    # Step 4: Verify first word
    set first [read_memory 0xFFFC0000 32 1]
    echo "First word at OCM: [format 0x%08X $first]"

    # Step 5: Set SP and PC, run FSBL
    reg sp 0xFFFF0000
    reg pc 0xFFFC0000
    echo "Jumping to FSBL at 0xFFFC0000..."
    resume

    echo "FSBL running. Watch serial console (ttyUSB1) for output."
    echo "After FSBL prints 'Flashing...' or similar, it will reflash QSPI."
    echo "Wait 30 seconds then power cycle."
}

recover
