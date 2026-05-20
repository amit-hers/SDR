# Program Zynq QSPI flash via JTAG
# QSPI flasher runs from OCM at 0x00020000
# Boot data loaded in 64KB chunks to 0x04000000 (OCM chunk buffer at 0x00040000)

set FLASHER_BIN "/home/parallels/Documents/SDR/recovery/qspi_flasher.bin"
set BOOT_BIN    "/home/parallels/Documents/SDR/tezuka-plutoplus-v0.3.5-7cf6171/sdimg/BOOT.bin"

# But BOOT.bin is 1.9MB - bigger than OCM.
# Strategy: load flasher to OCM 0x20000, data chunks to OCM 0x40000 (64KB at a time)
# The flasher reads from BOOT_DATA=0x04000000 - we need to change that to 0x00040000

echo "This approach requires patching the flasher for chunked operation."
echo "Using simpler approach: program QSPI directly via register writes from OpenOCD TCL."
