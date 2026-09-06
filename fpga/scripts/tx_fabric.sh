#!/bin/sh
# Bring up the FABRIC modulator for live byte streaming. Run ON the Pluto.
#   usage: tx_fabric.sh [sample_rate_Hz] [tx_lo_Hz] [diff_mode]
#
# This is the path that makes real payload streaming possible at all. Replaying
# pre-modulated IQ from the host needs 4 bytes per sample -- 69 MB/s at
# 17.28 MS/s -- which the USB-ethernet cannot carry. Feeding the fabric
# modulator instead sends one byte per 16 samples, about 1.08 MB/s, which it
# carries easily.
#
# Order below is not arbitrary; each step was established by measurement:
#
#  * DAC DATARATE (0x7902404C) defaults to 0, which makes dac_valid fire every
#    l_clk cycle. l_clk is 2*fs, so the DAC then plays out at twice the intended
#    rate: samples-per-symbol collapses 4 -> 2 and the symbol rate doubles. The
#    stock driver does not set it because sdr_insert.tcl removed the TX
#    interpolating FIR its rate calculation assumes. Invisible to the on-chip IQ
#    probe, which taps upstream of the DAC.
#  * The DMA IRQ mask is re-masked by every PL reload; unmask it or transfers
#    complete in hardware while userspace times out.
#  * Opening the buffer re-points the DAC channel at the internal DDS, so the
#    DMA source must be selected AFTER that, never before.
#  * The modem cores must run before ADC channels are enabled, or the sticky
#    overflow latch fires on the first sample and never clears.
set -e
FS=${1:-17280000}
LO=${2:-434000000}
DIFF=${3:-1}
AB=0x79020000; DB=0x79024000; T=0x7C420000; MOD=0x43C10000
PHY=/sys/bus/iio/devices/iio:device0
DDS=/sys/bus/iio/devices/iio:device3

echo "$FS" > $PHY/in_voltage_sampling_frequency
echo "$FS" > $PHY/out_voltage_sampling_frequency
echo "$LO" > $PHY/out_altvoltage1_TX_LO_frequency
echo 4000000 > $PHY/out_voltage_rf_bandwidth
echo 0 > $PHY/out_voltage0_hardwaregain
echo fdd > $PHY/ensm_mode

devmem $((AB+0x40)) 32 0x3           # ADC core out of reset: it gates all of l_clk
devmem $((DB+0x40)) 32 0x3           # DAC core out of reset
devmem $((T+0x80)) 32 0              # DMA IRQ unmask (PL reload re-masks it)
devmem $((DB+0x4C)) 32 1             # datarate: dac_valid = l_clk/(n+1), n=1

devmem $((MOD+0x10)) 32 1            # modulator enable
devmem $((MOD+0x18)) 32 0            # QPSK, not BPSK
devmem $((MOD+0x20)) 32 "$DIFF"      # differential, must match the demodulator
devmem $((MOD+0x00)) 32 0x81         # ap_start + auto_restart

# Zero every DDS tone or it overrides the DMA path entirely.
for a in out_altvoltage0_TX1_I_F1 out_altvoltage1_TX1_I_F2 out_altvoltage2_TX1_Q_F1 out_altvoltage3_TX1_Q_F2; do
  echo 0 > $DDS/${a}_raw 2>/dev/null || true
done

echo "# fs=$(cat $PHY/out_voltage_sampling_frequency) lo=$(cat $PHY/out_altvoltage1_TX_LO_frequency)"
echo "# l_clk_mon=$(devmem $((AB+0x54)) 32) datarate=$(devmem $((DB+0x4C)) 32)"
echo "# mod ap=$(devmem $((MOD+0x00)) 32) en=$(devmem $((MOD+0x10)) 32) diff=$(devmem $((MOD+0x20)) 32)"
echo "# ready for a BYTE stream on /dev/iio:device3"
