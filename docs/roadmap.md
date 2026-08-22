# Roadmap and recommended next steps

This roadmap orders work by dependency and risk. Stabilize the current
point-to-point bridge before adding more modulation schemes or networking
topologies; otherwise new features will amplify existing operational and
protocol uncertainty.

## Phase 1: make the current workflow reliable

### 1. Repair deployment and service scripts

- Change `scripts/deploy.sh` to use the actual binary path,
  `build/src/daemon/sdr-datalink`.
- Default deployment to `bw_mhz: 1` and fixed BPSK or QPSK instead of 10 MHz
  and `AUTO`.
- Generate unique node IDs and allow separate Pluto URIs for each host.
- Make `setup-service.sh` work from both a source checkout and a deployed
  installation.
- Add `--dry-run` so generated configurations and commands can be reviewed.

Done when a clean Debian/Ubuntu host can be installed, configured, deployed,
restarted, and removed using documented commands.

### 2. Unify daemon and dashboard runtime paths

- Use `stats_path` consistently rather than mixing config paths with
  hard-coded per-node filenames.
- Add a configurable reload path, or replace signal/file reload with a local
  control socket.
- Make the dashboard attach to an already-running service instead of assuming
  it owns the daemon process.
- Add authentication or bind to loopback by default.
- Remove or properly build `src/tools/live_stats.cpp`.

Done when start/stop, statistics, scanning, and live frequency/attenuation
changes work for one or two nodes without filename overrides.

### 3. Strengthen configuration handling

- Replace the minimal string-search JSON parser with a real JSON library.
- Reject malformed values instead of silently applying defaults.
- Validate frequency ranges, gain modes, MTU, scan bounds, and mode-specific
  options.
- Warn about ignored or unknown keys to catch spelling mistakes.
- Add `--check-config` and `--print-effective-config` commands.
- Provide clean example files under `configs/` rather than using lab-specific
  addresses as the main defaults.

Done when every configuration error produces a precise message before the
radio or network interface is opened.

### 4. Add continuous integration

- Build Debug and Release configurations on supported Linux versions.
- Run all eight unit suites with assertions enabled.
- Add AddressSanitizer and UndefinedBehaviorSanitizer jobs.
- Add formatting/static-analysis checks and Markdown link validation.
- Keep hardware tests separate and explicitly opt-in.

Done when every change receives a reproducible software-only quality signal.

## Phase 2: validate and measure the radio link

### 5. Build a repeatable hardware test harness

- Define a safe cabled RF setup, attenuation budget, radio firmware version,
  oscillator/reference arrangement, and test configuration.
- Automate ping, UDP loss, TCP throughput, latency, jitter, and bidirectional
  load tests.
- Capture configuration, daemon version, radio serial, temperature, and link
  statistics with every result.
- Establish pass/fail thresholds for BPSK and QPSK at 1 MHz before testing
  higher rates.

Done when another operator can reproduce the reported link performance from a
written procedure and obtain comparable results.

### 6. Improve observability

- Export Prometheus-compatible metrics or a documented stable JSON schema.
- Track packet latency, sequence gaps, duplicate frames, queue occupancy,
  carrier-sense deferral, and ARQ round-trip time.
- Separate radio loss, CRC/FEC failure, queue drop, TAP write failure, and
  transmit failure counters.
- Add structured logs with severity and timestamps.
- Record build/version information in stats and logs.

Done when a failed throughput run can be attributed to RF, DSP, scheduling,
medium access, or host networking using recorded metrics alone.

### 7. Test compatibility between modes and options

Create a matrix covering:

- bridge, mesh, P2P, and scan;
- BPSK and QPSK;
- FEC on/off, encryption on/off, and bridge ARQ on/off;
- USB and network libiio backends;
- one-way and bidirectional offered load.

Bridge mode currently contains the newest DSP and synchronization path. Either
bring mesh/P2P to feature parity through shared pipeline code or mark them
experimental until they pass the same tests.

## Phase 3: protocol and security improvements

### 8. Add capability negotiation

Introduce a robust control handshake that exchanges protocol version,
modulation, bandwidth, FEC, encryption, MTU, and feature flags. Keep the
acquisition/control channel on fixed BPSK.

This is a prerequisite for safe adaptive modulation: each transmitter should
choose a scheme based on feedback from its receiver, with explicit agreement
and fallback—not from its own local RSSI.

### 9. Add authenticated encryption

Replace unauthenticated AES-CTR with an AEAD construction such as AES-GCM or
ChaCha20-Poly1305. Define nonce construction, replay protection, key rotation,
and failure behavior in the wire protocol. Keep CRC for accidental channel
errors if useful, but do not treat it as authentication.

Because this changes frame overhead and compatibility, introduce a new frame
version and test mixed-version rejection.

### 10. Improve medium access and reliability

- Add randomized carrier-sense backoff to reduce synchronized collisions.
- Include destination/source addressing in control and ACK frames.
- Add duplicate suppression and replay windows.
- Tune ARQ from measured round-trip time and expose retry/backoff state.
- Consider fragmentation/reassembly rather than relying only on TAP MTU.
- Define behavior for multicast/broadcast and multiple peers.

Done when sustained bidirectional UDP traffic remains fair and stable without
one node monopolizing airtime.

## Phase 4: new features

### Adaptive modulation

After negotiation and receiver feedback exist, support conservative
BPSK/QPSK adaptation using measured packet error rate and SNR. Add 16QAM and
64QAM only after over-the-air validation and fallback testing.

### Real mesh networking

Turn the current TUN tunnel into a mesh by adding peer identity, neighbor
discovery, addressed frames, routing, TTL/hop limits, duplicate suppression,
and route expiry. A small established routing protocol may be safer than a new
custom one.

### Multi-radio and diversity support

- Multiple radio contexts per daemon
- RX selection or combining
- Separate control and data channels
- External reference/PPS status and synchronization reporting

### Better management interface

- Versioned local API for configuration and status
- Role-based authenticated remote access
- Configuration diff, validation, and rollback
- Spectrum/history export and downloadable diagnostic bundles

### Packaging

- `cmake --install` layout with example configs and service units
- Debian package and uninstall path
- Containerized dashboard only; keep hardware/TAP daemon native unless device
  and network privileges are deliberately handled
- Release versioning, changelog, license, and signed artifacts

### FPGA acceleration

Profile first, then move only proven bottlenecks into programmable logic.
Define and test a stable register/stream interface, bitstream compatibility
metadata, and a safe software fallback. The current HLS assets should remain
experimental until integrated bitstreams are reproducibly built and tested.

## Suggested next three deliverables

1. **Reliable local release:** fix scripts and paths, add strict config
   validation, provide sanitized example configs, and establish CI.
2. **Measured link release:** publish a reproducible hardware test procedure
   and baseline results for BPSK/QPSK at 1 MHz with complete metrics.
3. **Protocol v4 design:** specify capability negotiation, addressed ACKs, and
   authenticated encryption before implementation changes the wire format.

Avoid beginning adaptive modulation, high-order QAM, or real mesh routing
before these three deliverables. They depend on negotiation, trustworthy
metrics, and a reproducible baseline.
