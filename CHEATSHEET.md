# sdr-datalink — Cheatsheet

## Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Binary: `build/src/daemon/sdr-datalink`

Debug build:

```bash
cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dbg -j$(nproc)
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Single suite:

```bash
./build/tests/sdr-tests splitmod     # framing | fec | modem | crypto | dsp | arq | splitmod | aggregate
```

## Find your radios

Link-local IPs change on reboot. Resolve by serial:

```bash
iio_info -s | grep PLUTOPLUS
```

## Run

```bash
sudo ./build/src/daemon/sdr-datalink --config config.json
```

Two nodes on one host:

```bash
sudo ./build/src/daemon/sdr-datalink --config config.json        # node A
sudo ./build/src/daemon/sdr-datalink --config config_node2.json  # node B
```

Stop:

```bash
sudo pkill -x sdr-datalink
sudo ip link del sdr0      # remove stale TAP if startup fails with TUNSETIFF
```

## Network setup

```bash
sudo ip addr add 10.99.0.1/24 dev sdr0
sudo ip link set sdr0 up
sudo ip neigh replace 10.99.0.2 lladdr <peer-tap-mac> dev sdr0 nud permanent
cat /sys/class/net/sdr0/address     # this node's TAP MAC
```

Both TAPs on one host will short-circuit through the kernel and never touch the
radios. Put the second one in a namespace:

```bash
sudo ip netns add rxns
sudo ip link set sdr1 netns rxns
sudo ip netns exec rxns ip addr add 10.99.0.2/24 dev sdr1
sudo ip netns exec rxns ip link set sdr1 up
```

## Config

Edit `config.json` / `config_node2.json`.

### Radio

| key | default | notes |
|---|---|---|
| `pluto_ip` | — | IP or `usb:1.2.3` |
| `freq_tx_mhz` / `freq_rx_mhz` | 434 / 439 | peer must mirror these |
| `bw_mhz` | 1 | integer symbol rate in MHz; sample rate is `bw_mhz * samples_per_symbol` MSPS |
| `samples_per_symbol` | 4 | `2` for throughput experiments or `4` for validated compatibility |
| `tx_atten_db` | 10 | 0 = max power |
| `rx_bw_factor` | 1.4 | analog filter as a multiple of symbol rate |
| `modulation` | BPSK | `BPSK` or `QPSK` recommended. `AUTO` warns and is forced to BPSK |

### Link

| key | default | notes |
|---|---|---|
| `tx_duty_max` | 0.65 | max fraction of air this node uses; 0 disables |
| `carrier_sense` | true | defer TX while the peer is heard |
| `cfo_method` | fft | `fft` or `grid` |
| `rx_buffer_samples` | 262144 | smaller = tighter deadline but truncates frames |
| `rx_queue_depth` | 8 | buffers queued for the DSP thread |
| `fec` / `encrypt` / `arq` | false | must match on both nodes. FEC now genuinely
corrects (RS runs before the CRC verdict), but measured a net loss at 2 MHz:
it rescued 10% of damaged frames while ~14% parity overhead cost more than it saved. |

### System

| key | default | notes |
|---|---|---|
| `rt_priority` | 0 | SCHED_FIFO priority, needs root; 0 = off |
| `pin_cores` | false | pin capture/DSP threads |
| `stats_path` | `/tmp/sdr_stats.json` | give each node its own |
| `spectrum_interval_ms` | 200 | 0 disables the FFT display |

**Validated setup: 1 MHz, QPSK, `rx_bw_factor` 1.35 — ~800 kbps at 99% CRC.**

Timing recovery defaults to the fixed-point path (`SDR_TSYNC=fixed`). Live A/B
over coax: CRC 99.3% vs 55.3% for symsync_crcf, 1.9x the frames, CPU 31% vs 38.5%.

Over coax, set `tx_atten_db` around 25 with a 40 dB pad — at 10 dB the receiver
overloads and CRC collapses to 25%.

RF test harness lives in `scripts/rf/`: `datalink.sh` (two-node link test, `TCP=1`
for iperf3), `ab.sh` (alternating A/B), `plutoip.sh` (resolve radios by serial).
The radios need a route to 169.254.0.0/16 on the interface their RJ45 ports use;
put the address in the NetworkManager profile or it gets flushed.
2 MHz is unstable (identical runs vary ~4×).

## Logs

Live stats:

```bash
watch -n1 'python3 -m json.tool /tmp/sdr_stats.json'
```

Key fields: `frames_rx_good`, `frames_rx_bad`, `bursts_detected`, `dropped`, `rx_duty_pct`.

Per-frame log:

```bash
SDR_FRAME_LOG=/tmp/rx.txt sudo -E ./build/src/daemon/sdr-datalink --config config.json
grep '^\[GOOD\]' /tmp/rx.txt | wc -l
grep '^\[RX-HDR\]' /tmp/rx.txt | head
```

CPU profile (printed on exit):

```bash
SDR_PROFILE=1 sudo -E ./build/src/daemon/sdr-datalink --config config.json
```

Raw IQ capture (for offline analysis):

```bash
SDR_IQ_DUMP=/tmp/rx.bin SDR_IQ_DUMP_MB=400 sudo -E ./build/src/daemon/sdr-datalink --config config.json
```

## Environment variables

| var | values |
|---|---|
| `SDR_PROFILE` | `1` — per-stage CPU report on exit |
| `SDR_FRAME_LOG` | path — TX/RX frames, headers, CRC failures |
| `SDR_RAW_LOG` | path — demodulated bytes as hex |
| `SDR_IQ_DUMP` / `SDR_IQ_DUMP_MB` | path / size cap |
| `SDR_RX_CARRIER` | `ls+costas` (default), `ls`, `costas`, `none`. Measured at 2 MHz:
ls+costas 96.5% CRC, ls-only 17.3%, costas-only 39.8% — both stages are needed |
| `SDR_COSTAS_BW` | carrier loop bandwidth, default 0.004 (was 0.04; halves slip rate) |
| `SDR_TSYNC` | `fixed` (default) — fixed-point timing recovery; `liquid` — symsync_crcf; `freerun` — continuous, experimental |

## Web monitor

```bash
sudo pip3 install --break-system-packages flask flask-sock
python3 src/monitor/server.py --host 127.0.0.1 --port 8080 --config config.json
```

Then open `http://localhost:8080`. The server has unauthenticated control
endpoints; do not bind it to an untrusted network.

## Troubleshooting

| symptom | check |
|---|---|
| `TUNSETIFF failed` | stale TAP: `sudo ip link del sdr0` |
| `cannot connect to <ip>` | IP changed: `iio_info -s` |
| `setSampleRate rejected` | `bw_mhz` > 7 — over the 30.72 MSPS ceiling |
| frames sent, none received | peer's `freq_rx_mhz` must equal this node's `freq_tx_mhz` |
| `dropped` climbing | DSP not keeping up — lower `bw_mhz` |
| high `bursts_detected`, low `frames_rx_good` | channel saturated; lower `tx_duty_max` or offered load |

## Fabric modem (Pluto+ PL)

Separate path from the host daemon: the modem runs in the programmable logic and
the host sees bytes, not IQ. Full detail in [docs/fabric-modem.md](docs/fabric-modem.md).

Bring-up — order matters:

```bash
# on the board
sh watchdog_relax.sh 120          # stock is a 10 s timeout; a big scp outruns it
sh fpga_abi_check.sh 3 3          # refuses if the bitstream is incompatible
sh rx_framed.sh 17280000          # receive
sh tx_fabric.sh 17280000 434000000 1   # transmit from a byte stream
sh tx_feed.sh /tmp/stream.bytes
```

Registers:

```bash
devmem 0x43C00028 32 1     # demod diff_mode -- must match the modulator
devmem 0x43C10020 32 1     # mod   diff_mode
devmem 0x43C00018 32       # lock_count (bytes emitted)
devmem 0x43C00030 32       # mu_clamped -- ~1.5% is healthy, near 100% is pinned
devmem 0x43C50000 32       # identity MAGIC, expect 0x5344524C
```

Measure a link:

```bash
g++ -O2 -std=c++17 -I include -o framed_link_test \
    fpga/tools/framed_link_test.cpp build/src/core/libsdr_core.a -lliquid -lcrypto

./framed_link_test gen 64 tx.iq tx.bytes
./framed_link_test per cap.bin tx.bytes 81280 32768    # per-frame loss
./framed_link_test ser cap.bin tx.bytes 81280 32768    # byte/symbol error rate
```

**Pass the right packet size.** The byte grid is continuous only within one DMA
transfer; analysing a 1024-byte capture as 8192 reports 22% loss against a true
0.19%.

Recover a wedged capture device:

```bash
sh free_capture_dev.sh     # /dev/iio:device4 is single-open
```

## Release and deployment

Full detail in [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md).

```bash
./release/build-release.sh v1.3.0-dev1 dev      # no tag needed
git tag -a v1.3.0 -m "Validated release v1.3.0"
./release/build-release.sh v1.3.0 release       # refuses on a dirty tree
./release/mark-validated.sh v1.3.0 "PER 0.00% over 12709 frames"
./release/promote-golden.sh v1.3.0
./release/retention.sh --dry-run
```

Deploy:

```bash
./scripts/flash.sh UNIT-A .              # runtime; does NOT reboot
./scripts/flash.sh UNIT-A . --persist    # writes flash, survives power-off
./scripts/verify.sh UNIT-A .
./scripts/rollback.sh UNIT-A             # back to the golden release
```

`--persist` replaces the rootfs, so the board returns with **stock hostname,
USB serial, MAC, password (`analog`) and IP (`192.168.2.1`)** — which collides
with a second attached board.

## Board gotchas

| symptom | cause |
|---|---|
| every `0x43Cxxxxx` read bus-errors | stock PL loaded; the rootfs is a ramdisk and a reboot loses the bitstream |
| demod input railed at full scale regardless of RF | ADC channel format `0x71`; must be `0x51` on core 10.03 |
| TX plays at double rate, sps 4→2 | DAC datarate (`0x7902404C`) left at 0; must be 1 |
| DAC emits nothing, every register reads healthy | DMA source selected before the buffer was opened |
| DMA transfers complete but userspace times out | DMA IRQ re-masked by the PL reload |
| board resets mid-test, `/tmp` and `/lib/firmware` empty | watchdog; stock `-T 10` |
| one board unreachable, the other fine | both host NICs in `192.168.2.0/24` — add a `/32` route |
| `stat: not found`, `strtonum` unknown | busybox: use `wc -c <`, and no `strtonum` in awk |
| capture returns 0 bytes | buffer device held by a stale drain — `free_capture_dev.sh` |
| `write error: Operation not permitted` on a feed | writing into the RX device — indices shifted; resolve by name via `iio_lookup.sh` |
| feed runs but nothing is transmitted | stale feeder holds the TX device; it survives SIGKILL — `free_capture_dev.sh`, else reboot |
| probe reads all zeros | `iq_probe_read.sh` re-arms first; the feed must still be running |
