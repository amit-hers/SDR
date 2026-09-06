#!/bin/sh
# Known-good TX bring-up for the QPSK modem build. Run ON the Pluto.
#   usage: tx_bringup.sh [sample_rate_Hz] [tx_lo_Hz] [tx_gain_dB]
#
# Order matters throughout; every step here was established by measurement:
#
#  * DAC DATARATE (0x7902404C) defaults to 0, which makes dac_valid fire every
#    l_clk cycle. At fs = 7.68 MS/s, l_clk = 15.36 MHz, so the DAC then consumes
#    at 15.36 MS/s and plays the waveform out TWICE AS FAST: samples-per-symbol
#    collapses 4 -> 2 and the symbol rate doubles. The stock driver does not set
#    it because sdr_insert.tcl removed the TX interpolating FIR its rate
#    calculation assumes. This is invisible to the on-chip IQ probe, which taps
#    upstream of the DAC -- it took a second radio to find.
#  * The DMA IRQ mask is re-masked by every PL reload; unmask it or transfers
#    complete in hardware while userspace times out.
#  * The driver resets the DAC channel source to DDS when the buffer is enabled,
#    so the DMA source must be selected AFTER that, not before.
#  * The modem cores must run before ADC channels are enabled, or the sticky
#    overflow latch fires on the first sample and never clears.
set -e
# Load the PL first: this board's rootfs is a ramdisk, so a power cycle loses
# both the bitstream and /lib/firmware. Re-upload libre_dma.bin before running.
if [ -f /lib/firmware/libre_dma.bin ]; then
  echo 0 > /sys/class/fpga_manager/fpga0/flags
  echo libre_dma.bin > /sys/class/fpga_manager/fpga0/firmware
fi
FS=${1:-7680000}
LO=${2:-2400000000}
GAIN=${3:-0}
AB=0x79020000; DB=0x79024000; T=0x7C420000
PHY=/sys/bus/iio/devices/iio:device0
DDS=/sys/bus/iio/devices/iio:device3

echo "$FS" > $PHY/in_voltage_sampling_frequency
echo "$FS" > $PHY/out_voltage_sampling_frequency
echo "$LO" > $PHY/out_altvoltage1_TX_LO_frequency
echo "$GAIN" > $PHY/out_voltage0_hardwaregain
echo fdd > $PHY/ensm_mode

devmem $((AB+0x40)) 32 0x3           # ADC core out of reset (drives l_clk domain)
devmem $((DB+0x40)) 32 0x3           # DAC core out of reset
devmem $((T+0x80)) 32 0              # DMA IRQ unmask (PL reload re-masks it)

# l_clk = 2 * fs, and dac_valid rate = l_clk / (datarate + 1), so datarate = 1.
devmem $((DB+0x4C)) 32 1

devmem 0x43C10010 32 1               # modulator enable
devmem 0x43C10018 32 0               # QPSK (not BPSK)
devmem 0x43C10000 32 0x81            # ap_start + auto_restart

echo 1 > $DDS/scan_elements/out_voltage0_en
echo 1 > $DDS/scan_elements/out_voltage1_en
echo 4096 > $DDS/buffer/length
echo 1 > $DDS/buffer/enable
# AFTER buffer enable: the driver has just reset these to DDS.
for ch in 0 1; do
  devmem $((DB+0x418+64*ch)) 32 2    # channel source = DMA
  devmem $((DB+0x400+64*ch)) 32 1    # channel enable
done

LC=$(devmem $((AB+0x54)) 32)
echo "  fs=$FS lo=$LO gain=${GAIN}dB"
echo "  l_clk_mon=$LC  datarate=$(devmem $((DB+0x4C)) 32)  src=$(devmem $((DB+0x418)) 32)  irqmask=$(devmem $((T+0x80)) 32)"
echo "  mod AP=$(devmem 0x43C10000 32)  ready for a byte stream on /dev/iio:device3"

# ---- RX side: ADC channel control MUST include the data-format bits --------
# bit0 enable, bit4 dfmt_enable, bit5 dfmt_type, bit6 dfmt_se  =>  0x51
#
# With only bit0 set, axi_ad9361 passes raw OFFSET-BINARY to the demodulator,
# which expects signed two's complement. Every negative sample wraps positive
# and the constellation is destroyed.
#
# dfmt_type (bit 5) must be 0 on this core. Measured by sweeping the register
# with the RX IQ probe on the demodulator's own input, transmitter off, so the
# truth is amplified noise:
#
#   0x01 -> I in [0, 32760]        offset binary, all positive
#   0x31 -> I in [16128, 16648]    offset binary read as signed
#   0x51 -> I in [-232, +272]      CORRECT: sane signed noise
#   0x71 -> I in [-16384, 16368]   pinned on the 12-bit rails
#
# 0x71 was measured correct on an EARLIER ADC core; the core in this PL reports
# version 0x000A0300 (10.03) at 0x79020000, and with 0x71 it rails the demod
# input at 12-bit full scale regardless of RF -- identical with the transmitter
# off, at every RX gain and every TX power -- which looks exactly like a dead
# digital interface and is not (the PN monitor reads 0x0 under BIST INJ_RX).
# Re-measure these bits after any core version change; do not carry the value
# forward. With 0x51 the input is clean: rms 4465, peak 7648, 0% clipped.
rx_channels_on() {
  for ch in 0 1 2 3; do devmem $((0x79020400 + 64*ch)) 32 0x51; done
}
