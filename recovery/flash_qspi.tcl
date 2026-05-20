# OpenOCD TCL script: recover HamGeek Pluto+ by flashing QSPI from SD files
# Run after: openocd -f zynq7020_ft4232.cfg -f flash_qspi.tcl

proc recover_pluto {boot_bin pluto_dfu} {
    echo "=== Pluto+ JTAG QSPI Recovery ==="

    # Halt the CPU
    halt
    echo "CPU halted"

    # Initialize QSPI flash
    flash probe 0
    echo "Flash probed"

    # Write BOOT.bin to QSPI offset 0x0 (contains FSBL + U-Boot)
    echo "Writing BOOT.bin to QSPI 0x0..."
    flash write_image erase $boot_bin 0x0

    echo "QSPI boot sector written. Reset and check serial console."
    echo "If U-Boot comes up, run: run loaddfu (with pluto.dfu on SD card)"
    reset run
}

# Add QSPI flash bank (Spansion S25FL128S / compatible)
flash bank qspi.flash jtagspi 0 0 0 0 $_TARGETNAME

after 500
recover_pluto "/home/parallels/Documents/SDR/tezuka-plutoplus-v0.3.5-7cf6171/sdimg/BOOT.bin" \
              "/home/parallels/Documents/SDR/tezuka-plutoplus-v0.3.5-7cf6171/pluto.dfu"
