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

### 2. Unify daemon and dashboard runtime paths — PARTLY DONE

Done:

- ~~Add a configurable reload path.~~ `SDR_RELOAD_FILE`, alongside the existing
  `SDR_STATS_FILE` and `SDR_SCAN_FILE`. The daemon had hard-coded
  `/tmp/sdr_reload.json` while the dashboard wrote `/tmp/sdr_reload_<node>.json`,
  so a live retune went to a file nothing read and `SIGUSR2` appeared to do
  nothing for every named node.
- ~~Remove or properly build `src/tools/live_stats.cpp`.~~ Built as
  `sdr-live-stats`; it reads IQ from the radio with no TAP and no root.

Still outstanding:

- Use `stats_path` consistently rather than mixing config paths with per-node
  environment overrides. The override is now coherent and documented, but it is
  still two mechanisms for one thing.
- Replace signal/file reload with a local control socket, which would remove the
  path coordination problem rather than parameterising it.
- Make the dashboard attach to an already-running service instead of assuming it
  owns the daemon process.
- Add authentication or bind to loopback by default.

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

### 4. Add continuous integration — PARTLY DONE

`.github/workflows/ci.yml` builds, runs all nine unit suites with assertions
enabled, parses every shell script, builds a `dev` release bundle and asserts
that tampering with a bundled file is detected. `.github/workflows/release.yml`
rebuilds a bundle from a `v*` tag and attaches it to a GitHub Release.

Since added: AddressSanitizer and UndefinedBehaviorSanitizer jobs, a Debug
build, and `sdr-fuzz`, which feeds hostile input to the two decoders that take
data from outside -- the state protocol from the network and the deframer from
the air. It asserts that nothing unverifiable is ever accepted and that pure
noise never yields a frame. Mutation-tested: disabling the CRC check makes it
accept 2386 crafted packets and fail, so the gate can actually fire.

Still outstanding:

- More than one Linux version.
- Formatting and static-analysis checks.
- Hardware tests remain out of CI by necessity, not only by choice: Vivado
  cannot run on a GitHub runner and the licence is node-locked to a USB NIC,
  so the bitstream is an input to a release rather than a product of it.

Done when every change receives a reproducible software-only quality signal.

## Phase 2: validate and measure the radio link

### 5. Build a repeatable hardware test harness — DONE for the fabric modem

`fpga/tools/framed_link_test.cpp` plus the board scripts in `fpga/scripts/`
form a reproducible harness for the PL modem: numbered frames, per-frame loss,
byte/symbol error rate, cold-start acquisition and post-boundary re-acquisition.
The procedure, the measured results and the traps that invalidate a naive
measurement are written up in [Fabric modem](fabric-modem.md).

Established results: 7.85 Mbit/s of framed goodput at 0.00% PER, 17.28 MS/s,
1200-byte payloads, verified over a 16 s continuous run and a 121 s soak.

Still outstanding:

- The equivalent harness for the **host-side daemon** path, which is what the
  original item was about: ping, UDP loss, TCP throughput, latency, jitter and
  bidirectional load, with pass/fail thresholds for BPSK and QPSK at 1 MHz.
- Recording radio serial and temperature alongside each result.
- A written cabled-RF setup and attenuation budget.

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

### Communications and mission-platform feature backlog

The following 15 tasks expand the mesh and adaptive-modulation proposals.
Experimental implementations now exist in [Mission network](mission-network.md).
That document tracks software coverage and remaining integration/qualification
for each task; none is claimed as a qualified production release.
They target robust communications, navigation-safety data transport, sensing,
and mission-platform integration. The frozen [Phase 8 candidate](PHASE8-CANDIDATE.md)
remains the baseline; schedule these for subsequent development releases.

The host pipeline already has modulation/FEC primitives, while the production
fabric path uses QPSK. A host implementation does not qualify the FPGA path.
Extend existing diagnostics, traffic classes, handshake, and loop suppression
where applicable rather than treating those capabilities as absent.

**Delivery order:** mesh and relay → RF monitoring/history and channel control
→ modulation signaling and fixed 64-QAM characterization → fixed 16-QAM
characterization and automatic selection → FEC → cooperative awareness and
sense-and-avoid transport → security → redundancy → field qualification.
Design security and clock interfaces early: authenticated control is required
before operational automatic channel changes, and trusted time is required
before claiming accurate one-way latency. The order expresses development
priority, not permission to skip dependencies or security gates.

Every acceptance run follows [Testing rules](TESTING-RULES.md): prove distinct
node identities/endpoints, intended RF connectivity, and live stimulus before
scoring results; classify unmet preconditions as INVALID. Set measurable
latency, recovery, throughput, and error-rate budgets in each test plan before
testing. Publish configuration, build/FPGA identity, workload, duration,
measurements, and negative-path results. Hardware qualification is separate
from simulation and software tests.

#### F01. MANET / mesh networking

- Add neighbor discovery, addressed traffic, route selection, link-cost
  metrics, route expiry, hop limits, and duplicate suppression for 3+ nodes.
  Evaluate an established routing protocol before defining a custom one.
- Depends on capability negotiation and multi-peer medium access (Phase 3).
- **Done when:** three or more nodes discover routes automatically; node
  movement/loss causes bounded rerouting through an available alternate path;
  partitions expire stale routes and heal without loops. Expose neighbors,
  routes, costs, and convergence times through diagnostics.

#### F02. Mesh relay / range extension

- Forward A → B → C when A and C cannot communicate directly. Export hop
  count, per-hop latency, throughput, queue pressure, and link quality.
- Depends on F01; use bounded queues, hop limits, and duplicate suppression.
- **Done when:** a fixture proves the direct A–C path is unavailable and
  delivers traffic via B; relay loss, route loops, duplicate packets, and
  saturation produce bounded drops/recovery with visible counters. Record
  relay airtime cost and end-to-end goodput under bidirectional load.

#### F03. RF interference detection

- Monitor RSSI/noise floor, CRC/PER, acquisition quality, occupancy, and sudden
  degradation. Emit RF_INTERFERENCE, CHANNEL_DEGRADED, and RF_RECOVERED events
  with timestamps, thresholds, sample windows, and supporting measurements
  through the diagnostics API. Represent unavailable measurements explicitly.
- Depends on trustworthy link counters and a reproducible baseline.
- **Done when:** controlled interference and attenuation produce expected
  events and recovery, while a healthy link does not oscillate or trigger
  recovery. Distinguish RF evidence from congestion, peer silence, and DMA
  faults; an interference event is an evidence-based suspicion, not proof of
  an emitter's identity or intent.

#### F04. RF environment history and movement correlation

- Keep bounded time histories of degradation and classify stationary,
  intermittent, or changing conditions with confidence/unknown states.
  Correlate measurements with position/time when GNSS is available and
  expose the history without SSH.
- Depends on F03; use F14 for cross-node time correlation.
- **Done when:** controlled stationary/intermittent/changing traces are
  distinguishable; missing or stale position, clock jumps, and history
  overflow preserve usable RF records with explicit validity. Movement
  correlation must not be presented as interference-source localization.

#### F05. Automatic channel selection

- Evaluate only configured legal/authorized channels. Negotiate an agreed
  channel plan, transaction ID, switch point, acquisition deadline, and
  rollback/rendezvous behavior before retuning; preserve TX/RX frequency
  pairing. Define coordination for every affected neighbor in a mesh.
- Depends on F03–F04, capability negotiation, and F13 authenticated control
  for operational use; keep early experiments explicitly isolated.
- **Done when:** both peers agree and reacquire within the declared budget;
  lost proposals/ACKs, asymmetric reception, reboot during switching, and
  acquisition failure converge to a shared working plan or an explicit safe
  state. Reject unauthorized channels and prevent repeated channel flapping.

#### F06. Selectable modulation and robust signaling

- Support BPSK/QPSK/16-QAM/64-QAM per link or frame on each intended modem
  path. Keep acquisition/header signaling on a fixed robust mode with
  versioned payload modulation and capability fields.
- Depends on Phase 3 negotiation; F07 qualifies the FPGA 64-QAM payload path.
- **Done when:** supported payload modes decode using the robust header;
  corrupt headers, unsupported modes, and mixed versions reject safely.
  Publish a host/FPGA capability matrix and measured fixed-mode results;
  automatic selection remains gated on F08.

#### F07. Fixed 64-QAM FPGA modem path

- Implement mapping/demapping, normalization, decision regions, carrier/phase
  handling, and a documented differential/non-differential choice. Define
  fixed-point widths, clipping behavior, and register/stream compatibility.
- Depends on robust signaling in F06 and a reproducible QPSK baseline.
- **Done when:** reference vectors and RTL simulation cover all constellation
  points, decision boundaries, reset, and streaming backpressure; hardware
  runs publish EVM, constellation error, BER/PER, clipping, SNR, and goodput
  versus attenuation, including acquisition loss and recovery.
- Build and characterize **fixed 64-QAM first**, then fixed 16-QAM. Existing
  software QAM support is not evidence of FPGA or RF qualification. Retain
  the QPSK baseline until both new modes meet their declared budgets.

#### F08. Adaptive modulation and coding controller

- Choose mutually supported modulation/rate from receiver PER, EVM, SNR,
  RSSI, and retry history. Require sustained good conditions for upgrades
  and faster fallback; expose decisions, thresholds, dwell times, and cause.
- Depends on F03, F06, and fixed 64-QAM then 16-QAM qualification under F07;
  enable coding adaptation only after F09 is qualified.
- **Done when:** controlled fades, burst errors, stale/lost feedback, and
  asymmetric links cause bounded fallback without mode disagreement or
  flapping. Compare goodput/PER against fixed modes and verify a robust
  mutually supported fallback. Local RSSI alone must not drive TX upgrades.

#### F09. Configurable forward error correction

- Evaluate existing Reed-Solomon support against LDPC or another suitable
  scheme using decoder cost, latency, memory, and measured link benefit.
  Negotiate coding capabilities/rates and specify framing/interleaving.
- Depends on capability signaling; integrate selected rates with F08.
- **Done when:** correctable errors recover byte-exact payloads and
  uncorrectable/truncated blocks fail integrity checks; incompatible rates
  are rejected. Publish coding gain versus overhead/latency and decoder
  corrected/failed counts for every qualified host/FPGA path.

#### F10. Sense-and-avoid data interface

- Define a versioned low-latency message path for radar, lidar, camera,
  ADS-B, or other perception outputs, with source, timestamp, validity,
  expiry, and bounded payload size. Map messages to HIGH priority and expose
  queue delay, deadline misses, loss, and worst observed transport latency.
- Depends on traffic scheduling, F13 for operational trust, and F14 for
  one-way timing; transport does not itself implement collision avoidance.
- **Done when:** saturated video/bulk traffic, relay congestion, stale data,
  and link loss obey a declared latency budget or explicitly report deadline
  failure. Consumers can distinguish fresh, expired, and unavailable data;
  a finite test maximum is not claimed as a guaranteed worst-case bound.

#### F11. Multi-vehicle position / cooperative awareness

- Define a versioned low-rate message with node ID, position, velocity,
  heading, timestamp, validity, units, coordinate frame, and uncertainty.
  Maintain a bounded peer-state table separately from payload/video queues.
- Depends on F01 for multi-hop distribution, F13 for trusted identity, and
  F14 for age/clock uncertainty handling.
- **Done when:** multi-node updates respect rate limits; stale, reordered,
  duplicate, malformed, and implausible reports cannot silently replace
  valid fresh state. GNSS loss and peer disappearance expire entries and
  expose unknown state for deconfliction/formation-awareness consumers.

#### F12. Redundant link / failover support

- Support SDR as primary or backup alongside another interface, with
  degradation detection, bounded failover/failback, hysteresis, and explicit
  active-path state. Extend existing duplicate and L2 loop suppression.
- Depends on F03, addressed peer/link state, and F13 trust on both paths.
- **Done when:** outages, partial/asymmetric failures, and flapping switch
  paths within the configured budget; real frames returning through the
  alternate path are suppressed. Verify no forwarding loops or duplicate
  delivery under simultaneous path recovery, load, and reboot.

#### F13. Secure node authentication and encryption

- Add authenticated peer establishment, AEAD payload protection, replay
  windows, key provisioning/rotation/revocation, and secure management
  transport/access. Separate hardware identity from cryptographic identity;
  an existing bearer-token API is not encrypted transport.
- Extends Phase 3 security work; define protocol overhead and control-message
  authentication before finalizing mesh/channel/modulation wire formats.
- **Done when:** wrong/revoked keys, tampering, replay, mixed versions, failed
  rotation, and reboot cannot bypass authentication or reuse nonces. Failed
  authentication keeps forwarding disabled with no unsecured fallback;
  management access and authenticated recovery are tested independently.

#### F14. Time synchronization and mission clock

- Integrate GNSS/PPS/PTP or another trusted source, reporting source, offset,
  uncertainty, synchronization state, and holdover. Preserve monotonic time
  for deadlines and attach common-clock timestamps to measurements/events.
- Define clock interfaces early for F04, F10, and F11; select the actual
  source based on platform support and trust requirements.
- **Done when:** source loss, offset/drift, reboot, and clock steps expose
  degraded validity without breaking timers. Report one-way latency only
  with valid synchronization and its uncertainty; otherwise use RTT/local
  durations and mark one-way latency unavailable.

#### F15. Mission / tactical network profile and qualification

- Provide a validated profile for command/telemetry priority, video limits,
  multicast/broadcast policy, mesh routing, redundancy, authenticated nodes,
  degraded-link behavior, and explicit fail-safe states.
- Depends on qualification of the enabled F01–F14 capabilities. Reject
  unsupported combinations and document hardware/modem compatibility.
- **Done when:** reproducible mobility, interference/fading, node loss,
  congestion, reboot, and long-duration runs meet predeclared budgets for
  delivery, latency, convergence, failover, clock accuracy, and resource use.
  Include negative paths and safe-state/recovery evidence; publish duration,
  configuration, limitations, and release/bitstream IDs before promotion.

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
