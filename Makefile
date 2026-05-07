## SDR Data Link — Makefile
##
## Targets:
##   make link-all          Build TX+RX+test for host (x86)
##   make link-arm          Cross-compile for Zynq ARM (runs ON the PlutoSDR)
##   make link-deploy       Build + copy binaries to PlutoSDR via SSH
##   make link-webui        Start web management UI at http://localhost:8080
##   make link-test         Software DSP loopback (no hardware needed)
##   make link-fpga         Synthesize FPGA QPSK IP (requires Vivado HLS)
##   make datalink          Build Datalink daemon
##   make datalink-test     Run Datalink unit tests (no hardware needed)
##
## ── Performance levels ──────────────────────────────────────────────
##   Host mode (default):   DSP on PC, raw IQ over Ethernet  (80 MB/s)
##   ARM mode  (--arm):     DSP on Zynq ARM, only frames over Ethernet (5 MB/s)
##   FPGA mode (future):    DSP in FPGA fabric, ARM does framing only

CFLAGS    = -Wall -g -O2
CXXFLAGS  = -Wall -g -O2 -std=c++17 -pthread
LIBS      = -liio -lm
LINK_LIBS = -liio -lliquid -lssl -lcrypto -lm -lpthread

## Frequency targets
FREQ      = 98.0
LINK_FREQ = 434.0
PLUTO_IP  = 192.168.2.1

## ARM cross-compiler (for building binaries that run ON the Zynq)
ARM_CC    = arm-linux-gnueabihf-g++
ARM_FLAGS = -Wall -O2 -std=c++17 -pthread -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
## Libraries must be cross-compiled for ARM — see README_ARM.md
ARM_LIBS  = -liio -lliquid -lssl -lcrypto -lm -lpthread

.PHONY: build run run-only log fm fm-scan \
        link-all link-tx link-rx link-test link-bench \
        link-arm link-arm-tx link-arm-rx \
        link-deploy link-webui link-install \
        link-tx-run link-rx-run link-fpga \
        datalink datalink-test datalink-mesh datalink-l2bridge \
        datalink-p2p-tx datalink-p2p-rx datalink-scan datalink-ranging \
        clean

# ── rx_example (C, basic IQ test) ────────────────────────────────────
build:
	gcc $(CFLAGS) rx_example.c -o rx_example $(LIBS) && echo "✓ Build OK"

run: build
	./rx_example

run-only:
	./rx_example

log: build
	@mkdir -p logs
	./rx_example | tee logs/$(shell date +%Y%m%d_%H%M%S).log

# ── FM radio ─────────────────────────────────────────────────────────
fm: fm_radio.c
	gcc $(CFLAGS) fm_radio.c -o fm_radio $(LIBS) && echo "✓ FM Build OK"
	./fm_radio $(FREQ) 2>/dev/null | aplay -r 48000 -f S16_LE -c 1

fm-spectrum: fm_radio.c
	gcc $(CFLAGS) fm_radio.c -o fm_radio $(LIBS)
	./fm_radio $(FREQ) > /dev/null

fm-save: fm_radio.c
	gcc $(CFLAGS) fm_radio.c -o fm_radio $(LIBS)
	@mkdir -p logs
	timeout 30 ./fm_radio $(FREQ) 2>/dev/null | \
	  sox -t raw -r 48000 -e signed -b 16 -c 1 - \
	      logs/fm_$(FREQ)_$(shell date +%Y%m%d_%H%M%S).wav

fm-scan: fm_scan.c
	gcc $(CFLAGS) fm_scan.c -o fm_scan $(LIBS) && echo "✓ Scan Build OK"
	./fm_scan

# ── SDR data link — HOST x86 build ───────────────────────────────────
link-install:
	sudo apt install -y libliquid-dev libssl-dev python3-flask \
	                    gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf

link-all: link_tx link_rx link_test
	@echo "✓ All link binaries built (host)"

link_tx: link_tx.cpp
	g++ $(CXXFLAGS) link_tx.cpp -o link_tx $(LINK_LIBS) && echo "✓ link_tx OK"

link_rx: link_rx.cpp
	g++ $(CXXFLAGS) link_rx.cpp -o link_rx $(LINK_LIBS) && echo "✓ link_rx OK"

link_test: link_test.cpp
	g++ $(CXXFLAGS) link_test.cpp -o link_test -lliquid -lm -lpthread && echo "✓ link_test OK"

link-test: link_test
	./link_test 500 40.0

link-bench: link_tx
	dd if=/dev/urandom bs=1k count=500 | ./link_tx --freq $(LINK_FREQ)

link-tx-run: link_tx
	dd if=/dev/urandom bs=1k | ./link_tx --freq $(LINK_FREQ)

link-rx-run: link_rx
	./link_rx --freq $(LINK_FREQ) | pv > /dev/null

# ── SDR data link — ARM cross-compile (runs ON Zynq, no IQ over Ethernet) ──
##
## Before using:
##   1. sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
##   2. Cross-compile liquid-dsp for ARM:
##        git clone https://github.com/jgaeddert/liquid-dsp
##        cd liquid-dsp
##        ./bootstrap.sh
##        ./configure --host=arm-linux-gnueabihf --prefix=/opt/arm-sysroot
##        make -j4 && make install
##   3. Cross-compile libiio for ARM (or install on Zynq via opkg)
##
link-arm: link-arm-tx link-arm-rx
	@echo "✓ ARM binaries built — deploy with: make link-deploy"

link-arm-tx: link_tx.cpp
	$(ARM_CC) $(ARM_FLAGS) link_tx.cpp -o link_tx_arm \
	  -I/opt/arm-sysroot/include \
	  -L/opt/arm-sysroot/lib \
	  $(ARM_LIBS) && echo "✓ link_tx_arm OK"

link-arm-rx: link_rx.cpp
	$(ARM_CC) $(ARM_FLAGS) link_rx.cpp -o link_rx_arm \
	  -I/opt/arm-sysroot/include \
	  -L/opt/arm-sysroot/lib \
	  $(ARM_LIBS) && echo "✓ link_rx_arm OK"

# ── Deploy ARM binaries to PlutoSDR ──────────────────────────────────
##
## This copies the ARM binaries to the Zynq and starts them remotely.
## The Zynq then runs the DSP locally — only decoded frames cross Ethernet.
##
link-deploy: link-arm
	@echo "Deploying to $(PLUTO_IP)..."
	scp -o StrictHostKeyChecking=no link_tx_arm link_rx_arm \
	    root@$(PLUTO_IP):/usr/local/bin/
	@echo "✓ Deployed. SSH to device and run:"
	@echo "    link_rx_arm --freq $(LINK_FREQ) --sink udp --sink-ip <host_ip> --port 5006"
	@echo "    link_tx_arm --freq $(LINK_FREQ) --source udp --port 5005"

# ── On-device control via SSH ─────────────────────────────────────────
link-start-rx:
	ssh root@$(PLUTO_IP) \
	  "link_rx_arm --freq $(LINK_FREQ) --mod QPSK --sink udp --sink-ip $$(hostname -I | awk '{print $$1}') --port 5006 &"

link-start-tx:
	ssh root@$(PLUTO_IP) \
	  "link_tx_arm --freq $(LINK_FREQ) --mod QPSK --source udp --port 5005 &"

link-stop:
	ssh root@$(PLUTO_IP) "killall link_rx_arm link_tx_arm 2>/dev/null || true"

# ── Web management UI ────────────────────────────────────────────────
link-webui:
	cd webui && python3 server.py --port 8080

# ── FPGA synthesis (requires Vivado HLS 2019.1+) ─────────────────────
##
## The FPGA IP implements the entire DSP chain in hardware:
##   [AD9363 IQ] → [RRC filter + QPSK demod in FPGA fabric] → [ARM: framing only]
##
## Resources: ~800 LUTs, ~400 FFs, 2 DSP48 (Zynq-7010 has 80 DSP48 — plenty)
## Throughput: 200 MSPS (10× margin over 20 MSPS requirement)
##
link-fpga:
	@command -v vivado_hls >/dev/null 2>&1 || \
	  { echo "ERROR: vivado_hls not found. Install Vivado HLS 2019.1+"; exit 1; }
	cd fpga && vivado_hls -f hls_build.tcl
	@echo "✓ FPGA IP at fpga/qpsk_modem/solution1/impl/ip/"
	@echo "  Add to Vivado IP catalog, wire to cf-ad9361-lpc M_AXIS_0"

# ── Datalink daemon ──────────────────────────────────────────────────
##
## Adaptive mod (BPSK/QPSK/16QAM/64QAM), AES-256-CTR, RSSI ranging,
## IP mesh (TUN), L2 bridge (TAP), channel scan, P2P TX/RX, FDD
##
## Build:
datalink: datalink.cpp
	g++ $(CXXFLAGS) datalink.cpp -o datalink $(LINK_LIBS) && echo "✓ datalink OK"

## Run all unit tests (no hardware needed):
datalink-test: datalink_test.cpp
	g++ $(CXXFLAGS) datalink_test.cpp -o datalink_test -lliquid -lssl -lcrypto -lm \
	    && echo "✓ datalink_test built" && ./datalink_test

## Run specific test by number (e.g. make datalink-test-2):
datalink-test-%: datalink_test.cpp
	g++ $(CXXFLAGS) datalink_test.cpp -o datalink_test -lliquid -lssl -lcrypto -lm \
	    && ./datalink_test $*

## IP MANET mesh (FDD: TX on FREQ_TX MHz, RX on FREQ_RX MHz):
FREQ_TX       = 434.0
FREQ_RX       = 439.0
DATALINK_BW   = 10
DATALINK_NODE = 0x00000001

datalink-mesh: datalink
	sudo ./datalink --mode mesh \
	    --freq-tx $(FREQ_TX) --freq-rx $(FREQ_RX) \
	    --bw $(DATALINK_BW) --node-id $(DATALINK_NODE) \
	    --pluto-ip $(PLUTO_IP)

## L2 Transparent Bridge (TAP device, FDD):
datalink-l2bridge: datalink
	sudo ./datalink --mode l2bridge \
	    --freq-tx $(FREQ_TX) --freq-rx $(FREQ_RX) \
	    --bw $(DATALINK_BW) --node-id $(DATALINK_NODE) \
	    --pluto-ip $(PLUTO_IP)

## Unidirectional transmitter (P2P-TX):
datalink-p2p-tx: datalink
	sudo ./datalink --mode p2p-tx \
	    --freq $(LINK_FREQ) --bw $(DATALINK_BW) --mod AUTO \
	    --pluto-ip $(PLUTO_IP)

## Unidirectional receiver (P2P-RX):
datalink-p2p-rx: datalink
	sudo ./datalink --mode p2p-rx \
	    --freq $(LINK_FREQ) --bw $(DATALINK_BW) --mod AUTO \
	    --pluto-ip $(PLUTO_IP)

## Channel scan — sweep from FREQ MHz in 1 MHz steps, write /tmp/datalink_scan.json:
SCAN_FREQ = 430.0
SCAN_STEP = 1.0
SCAN_N    = 20

datalink-scan: datalink
	sudo ./datalink --mode scan \
	    --freq $(SCAN_FREQ) --scan-step $(SCAN_STEP) --scan-n $(SCAN_N) \
	    --pluto-ip $(PLUTO_IP)
	@echo "Scan results: /tmp/datalink_scan.json"

## RSSI-based ranging — print distance every second:
datalink-ranging: datalink
	sudo ./datalink --mode ranging \
	    --freq $(LINK_FREQ) --bw $(DATALINK_BW) \
	    --tx-power 30 --tx-gain 10 \
	    --pluto-ip $(PLUTO_IP)

# ── Clean ─────────────────────────────────────────────────────────────
clean:
	rm -f rx_example fm_radio fm_scan \
	      link_tx link_rx link_test \
	      link_tx_arm link_rx_arm \
	      datalink datalink_test
