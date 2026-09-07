# SDR Datalink

SDR Datalink is a C++20 burst-mode radio link for ADALM-Pluto-compatible
radios. Its primary mode carries Ethernet frames between Linux TAP interfaces;
the repository also contains routed TUN, UDP tunnel, channel-scan, monitoring,
FPGA, and device-recovery components.

> Project status: experimental hardware software. Not a plug-and-play
> production bridge. Start at low TX power (high attenuation), use suitable RF
> isolation or attenuators for cabled tests, and comply with local spectrum
> regulations.
>
> Two paths exist and are validated separately. The **host-side daemon** (the
> bulk of this README) modulates in software over libiio; its checked-in
> two-radio configuration was validated at a 1 MHz symbol rate with QPSK. The
> **fabric modem** runs the modulator and demodulator in the Pluto+
> programmable logic and has been measured at **7.85 Mbit/s of framed goodput
> at 0.00% frame loss** over the air — see [Fabric modem](docs/fabric-modem.md).

## What is implemented

- `bridge`: bidirectional Layer-2 transport through a Linux TAP device.
- `mesh`: bidirectional Layer-3 transport through a Linux TUN device. Despite
  its name, the current implementation is a packet tunnel, not a routed mesh
  protocol.
- `p2p-tx` / `p2p-rx`: one-way UDP-to-RF and RF-to-UDP transport on UDP port
  5005.
- `scan`: measures a configured series of receive frequencies and writes
  `/tmp/sdr_scan.json`.
- Optional AES-256-CTR encryption, Reed-Solomon FEC, and bridge-mode ARQ.
- Unconditional payload scrambling (additive 15-bit LFSR), required by the
  fabric modem's differential QPSK — see
  [Architecture](docs/architecture.md#payload-scrambling).
- JSON statistics plus a Flask/WebSocket monitoring dashboard.
- A QPSK modem in the Pluto+ programmable logic, with an on-chip identity block
  so software refuses to run against an incompatible bitstream.
- A versioned release and deployment system: one bundle carries FPGA, boot,
  kernel, rootfs, software, config and scripts, and one command restores it to a
  board — including power-on persistence.
- Vivado HLS blocks and Pluto+ recovery utilities.

## Requirements

- Linux with TUN/TAP support
- CMake 3.20+, a C++20 compiler, and pthreads
- libiio, liquid-dsp, and OpenSSL development packages
- An ADALM-Pluto or compatible Pluto+ radio
- Root or the appropriate capabilities for TAP/TUN and real-time scheduling
- Python 3, Flask, and Flask-Sock for the optional dashboard

On Debian or Ubuntu, the repository helper installs the required packages:

```bash
./scripts/install-deps.sh
```

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The daemon is created at `build/src/daemon/sdr-datalink`, and the test runner
at `build/tests/sdr-tests`. Hardware is not required for the unit tests.

## Configure two radios

Copy `config.json` for node A and reverse the frequencies for node B. The two
ends must use matching modulation, bandwidth, FEC, encryption, and key
settings.

```json
{
  "mode": "bridge",
  "pluto_ip": "usb:1.2.3",
  "node_id": "0x00000001",
  "freq_tx_mhz": 434.0,
  "freq_rx_mhz": 439.0,
  "bw_mhz": 1,
  "tx_atten_db": 30.0,
  "gain_mode": "fast_attack",
  "modulation": "QPSK",
  "tap_iface": "sdr0",
  "bridge_iface": "",
  "stats_path": "/tmp/sdr_stats_A.json",
  "tx_duty_max": 0.65,
  "carrier_sense": true,
  "rx_bw_factor": 1.35,
  "fec": false,
  "encrypt": false,
  "arq": false
}
```

For node B, set a unique `node_id` and TAP name, swap 434/439 MHz, and use a
different stats path. Discover libiio URIs with:

```bash
iio_info -s
```

An explicit `usb:` URI is preferred when available. A plain IP is accepted;
the radio layer attempts to resolve it to an unambiguous USB device.

`bw_mhz` is the symbol rate, not the I/Q sample rate. The daemon samples at
`bw_mhz * samples_per_symbol`; each sample is four bytes of interleaved I/Q.
The default 1 MHz and 4 samples/symbol therefore means 4 MSPS and 16 MB/s.
Two samples/symbol is available for throughput experiments, while four remains
the validated compatibility setting. Sustained traffic much above 32 MB/s
commonly exceeds reliable USB 2.0 reception; the boards' real Ethernet backend
may sustain more.

`AUTO` is not supported. If configured, it is deliberately changed to BPSK at
runtime. BPSK and QPSK are the recommended choices; 16QAM and 64QAM exist in
the wire format but have not been validated over the air in this project.

See [Configuration](docs/configuration.md) for every key and validation rule.

## Run

Start one process per radio:

```bash
sudo ./build/src/daemon/sdr-datalink --config config.json
sudo ./build/src/daemon/sdr-datalink --config config_node2.json
```

In bridge mode the program creates a TAP interface but does not assign IP
addresses or automatically attach it to a Linux bridge. For a direct routed
test between two machines:

```bash
# node A
sudo ip addr add 10.99.0.1/24 dev sdr0
sudo ip link set sdr0 up

# node B (use its configured TAP name)
sudo ip addr add 10.99.0.2/24 dev sdr1
sudo ip link set sdr1 up
```

Then test with `ping 10.99.0.2` and `iperf3`. For a transparent LAN bridge,
attach the TAP and LAN interface to a Linux bridge using your distribution's
network manager; take care not to disconnect the management interface.

## Monitor

The daemon itself writes statistics to `stats_path`. Inspect them directly:

```bash
watch -n1 'python3 -m json.tool /tmp/sdr_stats_A.json'
```

Or run the dashboard from the repository root:

```bash
python3 src/monitor/server.py --port 8080 --config config.json
```

Open `http://localhost:8080`. The dashboard can launch a local daemon, which
requires passwordless non-interactive `sudo` when the server is not root.
Its current multi-node defaults use `config.json` and `config_node2.json`, but
its default stats filenames differ from the checked-in configs; align the
`stats_path` values before relying on dashboard data.

## Documentation

- [Architecture](docs/architecture.md) — components, data flow, and frame format
- [Configuration](docs/configuration.md) — complete configuration reference
- [Operations](docs/operations.md) — setup, monitoring, diagnostics, and troubleshooting
- [Fabric modem](docs/fabric-modem.md) — the PL QPSK modem: measured throughput,
  register map, bring-up order, measurement tools, and the traps that produced
  more than one false conclusion
- [Deployment](docs/DEPLOYMENT.md) — building, tagging, flashing, verifying,
  rolling back, and what the ABI fields mean
- [Roadmap](docs/roadmap.md) — recommended next steps and future features
- [FPGA and recovery](docs/fpga-and-recovery.md) — HLS build and board-recovery assets
- [Cheatsheet](CHEATSHEET.md) — compact commands for established developers

## Important repository caveats

- `scripts/deploy.sh` currently defaults to 10 MHz and writes `AUTO`
  modulation. Both conflict with the validated settings; pass `--bw 1`, and
  review the generated config before using the script.
- `scripts/setup-service.sh` expects the in-tree build layout. The copy made by
  `deploy.sh` has a different layout, so service deployment should be reviewed
  before use.
- `src/tools/live_stats.cpp` exists but is not a CMake target. Statistics are
  produced by the daemon; commands referring to `build/live_stats` are stale.
- The monitor writes per-node reload files, while the controller currently
  reloads only `/tmp/sdr_reload.json`. Live tuning therefore needs that exact
  path and `SIGUSR2` until the implementations are unified.
- The checked-in `config.json` and `config_node2.json` are local lab examples
  containing link-local device addresses and intentionally different TX
  attenuation. Replace them for your hardware.

## License

No license file is present. Unless the repository owner adds one, no license
to copy, modify, or redistribute the code is granted by default.
