# Operations guide

## Safe bring-up

1. Give every radio a unique identity and confirm it appears in `iio_info -s`.
2. Use antennas appropriate for the selected band, or a cabled connection with
   enough attenuation and DC blocking for the hardware involved.
3. Begin with high TX attenuation (for example 60–89 dB for a cabled bench
   setup) and increase power only as needed.
4. Start at `bw_mhz: 1`, with BPSK or QPSK and FEC/encryption/ARQ disabled.
5. Cross the TX/RX frequencies and ensure both nodes agree on all wire options.
6. Start the daemons, bring up TAP/TUN interfaces, then test ping before bulk
   traffic.

Do not transmit outside frequencies and power levels authorized in your
jurisdiction.

## Radio identity and transport

Two Pluto+ devices may ship with an empty identical U-Boot `UniqueID`, causing
MAC and discovery collisions. To assign a persistent identity, connect only
one affected board and use its firmware environment tools:

```bash
ssh root@192.168.2.1
fw_setenv UniqueID MY-PLUTO-01
reboot
```

Repeat with a different ID for the second radio. Confirm the resulting unique
USB URI with `iio_info -s`. Firmware layouts vary; verify that `fw_setenv` is
supported before changing device environment data.

## Build variants

```bash
# Release
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# Debug
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j"$(nproc)"
```

Disable tests with `-DBUILD_TESTS=OFF`. Run one suite directly with:

```bash
./build/tests/sdr-tests framing
```

Valid suite names are `framing`, `fec`, `modem`, `crypto`, `dsp`, `arq`,
`splitmod`, and `aggregate`.

## TAP/TUN networking

The daemon creates the virtual interface; administrators configure addresses,
routes, and bridges. For a simple two-machine bridge-mode test:

```bash
# A
sudo ip addr add 10.99.0.1/24 dev sdr0
sudo ip link set sdr0 up

# B
sudo ip addr add 10.99.0.2/24 dev sdr1
sudo ip link set sdr1 up
```

If ARP discovery is unreliable during early RF tests, inspect each TAP MAC at
`/sys/class/net/<name>/address` and add a temporary static neighbor entry:

```bash
sudo ip neigh replace 10.99.0.2 lladdr PEER_MAC dev sdr0 nud permanent
```

Running two TAP daemons in the same network namespace can let Linux deliver
traffic locally instead of exercising RF. Put one interface/process in a
separate network namespace for a meaningful single-host test.

To make a true transparent bridge, use NetworkManager, systemd-networkd, or
`ip link` to create a Linux bridge and enslave both the LAN interface and TAP.
Plan management connectivity first: moving the host's active LAN address can
end an SSH session.

## Statistics and monitor

The daemon atomically exports a JSON snapshot to `stats_path`. Useful fields
include frame and byte totals, `bursts_detected`, `bursts_demodulated`,
`dropped`, RX duty cycle, RSSI/SNR, throughput, FEC counters, ARQ counters, and
the optional spectrum.

```bash
watch -n1 'python3 -m json.tool /tmp/sdr_stats_A.json'
python3 src/monitor/server.py --host 127.0.0.1 --port 8080 --config config.json
```

Binding the dashboard to `127.0.0.1` is recommended unless remote access is
required. The server has control endpoints and no authentication; do not
expose it to an untrusted network.

## 7 Mbit/s experimental benchmark

The repository includes a one-host/two-radio A-to-B benchmark. It uses a
separate network namespace for node B so the kernel cannot bypass RF, fixed
QPSK at 5 Msym/s, two samples/symbol, and an 85% one-way duty cap:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo ./scripts/benchmark-local.sh --duration 30
```

Review `configs/throughput_a.json` and `configs/throughput_b.json` first,
especially radio addresses, frequencies, and attenuation. The provided files
match the currently discovered lab radios and keep node B at 89 dB TX
attenuation for a one-way test.

For a 1200-byte aggregate, QPSK carries roughly 1.82 payload bits per symbol
after acquisition, frame, CRC, and postamble overhead. At 5 Msym/s and 85%
duty the resulting theoretical ceiling is approximately 7.7 Mbit/s before
loss and host-side overhead. Therefore a measured 7 Mbit/s is plausible but
not guaranteed. This profile is experimental; the compatibility profile
remains 1 Msym/s and four samples/symbol.

## Logging and profiling

```bash
SDR_PROFILE=1 sudo -E ./build/src/daemon/sdr-datalink --config config.json

SDR_FRAME_LOG=/tmp/sdr-frames.log \
  sudo -E ./build/src/daemon/sdr-datalink --config config.json

SDR_IQ_DUMP=/tmp/sdr-iq.bin SDR_IQ_DUMP_MB=256 \
  sudo -E ./build/src/daemon/sdr-datalink --config config.json
```

I/Q dumps can be large and may contain received signals unrelated to this
application. Store and share them accordingly.

## Troubleshooting

| Symptom | Likely cause and action |
|---|---|
| Cannot connect to radio | Run `iio_info -s`; update `pluto_ip`; fix duplicate identities/IP/MAC addresses |
| Sample-rate setting rejected | `bw_mhz * 4` exceeds the radio/firmware limit; reduce `bw_mhz` |
| Frames TX but no bursts RX | Swap/check peer frequencies, antennas/cabling, attenuation, and RX gain mode |
| Bursts detected but no good frames | Confirm modulation/FEC/encryption/key match; start with BPSK and features off |
| `dropped` grows | DSP is behind capture; reduce bandwidth, disable spectrum, enable profiling, or tune scheduling |
| RX duty is low | USB/backend cannot sustain the requested sample rate; use an explicit USB URI and 1 MHz |
| Channel appears continuously busy | Offered load or two transmitters saturate airtime; lower `tx_duty_max` and keep carrier sense enabled |
| `TUNSETIFF` fails | Run as root/capable user, check interface name collision, and remove only the confirmed stale interface |
| Monitor is empty | Make monitor and daemon stats paths agree; the current defaults are inconsistent |
| Dashboard live tuning has no effect | Controller reads `/tmp/sdr_reload.json`, not the monitor's per-node reload filename |

## Deployment scripts

The scripts are convenience scaffolding rather than a fully validated
installer. Before remote deployment:

- review `scripts/deploy.sh` and pass `--bw 1`;
- replace its generated `AUTO` modulation with BPSK or QPSK;
- verify its expected binary path (`build/sdr-datalink`) against the actual
  CMake output (`build/src/daemon/sdr-datalink`);
- verify `scripts/setup-service.sh` paths after files are copied remotely;
- review generated systemd units and configuration locations.

For reliable service operation, install the CMake target into an explicit
prefix and create units referring to stable absolute paths.
