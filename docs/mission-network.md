# Experimental mission network

The mission service implements an authenticated network above either UDP links
(including an existing SDR Ethernet/IP bridge) or the host SDR frame PHY.
It is a separate executable from the frozen appliance candidate. It does not
replace the production FPGA bitstream or make a flight-safety qualification claim.

## Implementation coverage

| Roadmap task | Implemented path | Qualification still needed |
|---|---|---|
| F01 mesh | Authenticated discovery of provisioned peers; path-vector routes, costs, expiry, alternate routing, hop limits | Mobile radios, airtime fairness and larger networks |
| F02 relay | Multi-hop forwarding, bounded queues, deduplication, hop accounting, per-neighbor RTT and counters | Per-hop RF throughput and latency envelope |
| F03 interference | RF sample/history API; degradation, suspected-interference and recovery events | Calibrated RSSI/noise measurements and interference discrimination on hardware |
| F04 environment | Bounded time/position histories and stationary/intermittent/changing classification | Motion-correlated field traces; no emitter localization |
| F05 channel selection | Authorized channel IDs, authenticated all-neighbor agreement, scheduled retune, acquisition probes, renewable leases and home-channel rollback | Radio retune/acquisition timing, channel survey input, clock accuracy |
| F06 modulation | Per-frame host BPSK/QPSK/16-QAM/64-QAM; robust BPSK header; QAM residual carrier tracking | Fixed-mode OTA measurements before adding a mode to capabilities |
| F07 FPGA 64-QAM | Synthesizable Gray mapper/demapper, normalization, hard decisions, error/clipping outputs; RTL tests | Integration with FPGA acquisition, timing/carrier recovery, pulse shaping, stream packing and bitstream timing closure; hardware EVM/BER/PER sweeps |
| F08 AMC | Receiver feedback, capability intersection, sustained upgrades, rapid fallback, stale-feedback reset | RF threshold calibration; higher-order modes default disabled |
| F09 FEC | Existing RS(255,223) integrated per frame and negotiated; coding rate 223/255 or uncoded; decoder counters | FPGA FEC implementation and measured RF coding gain |
| F10 safety interface | Priority safety messages with age/uncertainty checks, expiry, bounded queues and deadline counters | Sensor adapters and a system-level safety latency budget |
| F11 awareness | WGS84 position/ENU velocity messages, peer table, ordering, expiry and rate limits | Real platform position feeds and deconfliction application |
| F12 redundancy | Bidirectional path probes, sticky failover, shared replay/deduplication state; optional Ethernet tunnel and reflection suppression | TAP/RF/alternate-interface loop and failover hardware tests |
| F13 security | Pairwise key provisioning, fresh challenge sessions, HKDF/AES-GCM, replay protection, key reload, private management API | Independent protocol review, deployment key custody and forward secrecy |
| F14 clock | Monotonic deadlines, common-clock samples with uncertainty/holdover, chrony input process | GNSS/PPS/PTP deployment and measured source accuracy |
| F15 profile | Generated bounded profiles, safe state, video limits, broadcast policy, process and fault/soak harnesses | Long-duration physical field qualification and release promotion |

These are experimental implementations, not 15 completed production releases.
The FPGA symbol blocks alone are **not** a complete 64-QAM modem. They are not
wired into the production QPSK HLS design, whose carrier recovery and fixed-point
range cannot simply be reused for high-order QAM.

## Build and local three-node run

The host needs the normal C++ build dependencies and Python `cryptography`.
The service requires Python 3.10 or later. Install its Python dependency in your
normal environment from `src/mission/requirements.txt`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
scripts/mission-config.py --output /tmp/mission-demo --nodes 3
scripts/sdr-mission --config /tmp/mission-demo/node1.json
# In separate terminals:
scripts/sdr-mission --config /tmp/mission-demo/node2.json
scripts/sdr-mission --config /tmp/mission-demo/node3.json
```

Generated profiles form A–B–C, with no A–C socket path. Provisioning generates
independent random 256-bit keys for each adjacent pair. Files are mode 0600;
runtime directories are mode 0700. Runtime instances use an exclusive lock and
reject unsafe directory permissions. Do not put these generated keys in Git.
UDP endpoints are explicit IPv4 addresses/ports. Neighbor discovery, liveness,
route selection and rerouting happen automatically within the provisioned trust
set; an unknown radio is not automatically trusted.

`status.json` in each runtime directory is replaced atomically once a second.
The local datagram API is `api.sock` in that same directory. A bound Unix client
can submit a request and receive one JSON response:

```python
import base64, json, socket, tempfile
from pathlib import Path
with tempfile.TemporaryDirectory() as d:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(str(Path(d) / 'client'))
    s.settimeout(2)
    request = {'command': 'send', 'destination': 3, 'service': 'data',
               'payload': base64.b64encode(b'hello through relay').decode()}
    s.sendto(json.dumps(request).encode(), '/tmp/mission-demo/node1/api.sock')
    print(json.loads(s.recv(65536)))
```

At node 3, `{"command":"receive"}` drains delivered messages. The returned
payload is base64. `send` confirms local admission, not remote delivery; this
version does not add application acknowledgements/retransmission. Consumers
must handle loss explicitly.

## Local API and diagnostics

All control commands require access to the private Unix socket. There is no
unauthenticated TCP management listener. Status files contain no keys.

| Command | Fields / behavior |
|---|---|
| `status` | Versioned node/route/peer, RF history/events, queues, counters, clock, channel and position state |
| `send` | `destination`, `service`, base64 `payload`, optional `ttl_ms` (1–10000) |
| `receive` | Drain up to 128 retained messages; overflow is counted |
| `rf_sample` | `sample`: optional `rssi_dbm`, `noise_dbm`, `per`, `snr_db`, `evm` (ratio), `occupancy`, `acquisition`, `retry_rate`, `position: [lat,lon]`, and `peer` |
| `channel_score` | Authorized `channel` ID and nonnegative `cost`; lower is better; expires after 30 s |
| `channel` | Propose an authorized `channel` for a bounded trial |
| `clock` | Trusted `source` (`GNSS`, `PPS`, `PTP`, `NTP`), `utc_ns`, `uncertainty_ns` at receipt |
| `safe` / `resume` | Disable/enable payload forwarding; control and health probes continue |
| `reload_keys` | Reload keys from the original config path; reject other configuration changes; discard sessions/routes/queued payloads and authenticate again |

The mission API is separate from the board's `sdr-agent` API. RF measurements
must be supplied by the PHY or a local measurement adapter. Missing values stay
unknown. A sample without `peer` contributes to RF diagnostics but **cannot**
steer any peer's modulation. Samples with a provisioned transmitter identity
are returned as authenticated feedback to that transmitter.

Degradation uses consecutive bad samples and recovery uses three good samples.
Suspected interference additionally needs rising noise and high occupancy.
History classifications describe measured conditions, not the location or
intent of an interferer. Histories, events, peer tables, queues and duplicate
caches are bounded. Full duplicate caches drop new traffic rather than evict
still-live suppression entries.

## Radio frame adapter

`sdr-mission-phy` owns the radio exclusively. Stop another daemon using that
radio before starting it. It uses the existing host `SoftwarePhy`; it does not
require root unless the underlying device permissions do.

```text
sdr-mission-phy RADIO_CONFIG SOCKET SERVICE_SOCKET CHANNEL_FILE HOME_ID
```

`RADIO_CONFIG` is the existing validated radio JSON, with a unique `node_id`
matching the mission profile (mismatches disable forwarding),
`encrypt: false` (mission AEAD protects the payload), and the intended sample
rate/attenuation. The adapter enables RS receive capability; each transmit
frame independently chooses uncoded or RS. Acquisition, headers and mission
control frames use BPSK. Payload modulation is selected only from negotiated,
locally qualified capabilities.

`CHANNEL_FILE` lists **operator-authorized** `id tx_hz rx_hz` rows. Configure
crossed frequency pairs at peers, or a shared channel with suitable medium
access for multiple nodes. The software cannot infer regulatory authorization.
Create both socket parent directories mode 0700 owned by the service account.
Set `phy_socket` in the mission profile and use `"rf"` in the desired peer
`paths`. The adapter's `SERVICE_SOCKET` is `runtime_dir/frames.sock`.

Adapter status distinguishes requested channel state from the last acknowledged
physical channel. Payload transmission waits for a home-channel acknowledgement
and stops again if adapter heartbeats go stale. A radio-adapter restart during
a trial invalidates acquisition evidence.

The adapter exposes decision-directed EVM and an EVM-derived SNR estimate;
these are uncalibrated estimates, not laboratory RF SNR. Per-peer packet-loss
feedback uses frame sequence gaps, not the legacy RSSI-offset estimate.
Clipping and FEC counters are reported separately. Missing measurements do not
permit QAM upgrades. Measurement injection through `rf_sample` is useful for
reproducible control testing, but is not hardware evidence.

Channel selection requires a synchronized clock with uncertainty at most 5 ms,
fresh current/alternative channel scores and an authenticated neighbor clique.
The lowest node ID coordinates; all affected neighbors must agree. The switch
starts two seconds after proposal; an initial trial lasts ten seconds. An
adapter acknowledgement precedes the TRIAL state. Failed acquisition or lost
clock validity returns to the configured home channel. A monotonic watchdog
bounds recovery even if UTC steps backwards. With `auto_channel: true`, the
leader can extend the lease only while hearing current acquisition probes from
all members; lost extensions or leader loss eventually return nodes home.
The adapter independently returns home after two seconds without a service
heartbeat. In a general multi-hop topology, a proposal that omits one of a
participant's neighbors is refused; network-wide channel partitioning is not
implemented.

A retune stops and recreates the host PHY to discard queued old-channel samples.
Actual USB/IIO stop/restart time and reacquisition must be measured before
channel switching can be qualified. There is no automatic scanning across
unconfigured channels and no speculative RF transmission in the local tests.

## Traffic, safety and cooperative awareness

Services `command`, `telemetry`, `safety` and `position` use HIGH priority (64
queued messages, 20 ms local queue deadline). `data`, `video` and `ethernet` use
a 128-message queue with a 500 ms deadline. Video has a configurable token bucket
(`video_bytes_per_second`, default 32000, allowing up to one second of burst).
Safety/position require a usable mission clock; packets whose age plus clock
uncertainty exceeds their deadline are dropped. Queue limits cannot guarantee
an end-to-end safety deadline through an overloaded or disconnected radio.
The diagnostics report deadline misses and the largest observed accepted-packet
one-way age upper estimate; this is not a guaranteed worst-case latency.

The application payload limit is 400 bytes. The service has an eight-hop limit.
Destination 0 is controlled flooding, available only to safety/position when
`broadcast` is explicitly enabled. There is no arbitrary payload/video flood.

Position payloads use this JSON schema (under 400 bytes):

```json
{"node_id":1,"lat":32.0,"lon":34.0,"alt_m":10,"velocity_mps":[0,0,0],"heading_deg":90,"utc_ns":1800000000000000000,"valid_ms":1000,"valid":true}
```

Coordinates are WGS84 latitude/longitude in degrees, altitude in metres above
the WGS84 ellipsoid, velocity East/North/Up in m/s, heading clockwise from true
north. `valid:false` explicitly marks unavailable navigation state. Entries
expire; reordered/future/malformed reports are rejected; updates are limited
to 10 Hz per origin. The service transports perception/awareness data; it does
not decide how a vehicle should avoid an obstacle.

## Time and redundant Ethernet

Run `scripts/sdr-mission-clock --socket /path/to/api.sock` to import an already
synchronized chrony clock. It rejects unsynchronized/local-only references and
carries a conservative error estimate using the
[chrony tracking formula](https://chrony-project.org/doc/4.4/chronyc.html).
Chrony must be configured separately with trusted sources; this helper does
not change the system clock configuration. GNSS/PPS/PTP adapters can use the
same private `clock` API with their measured uncertainty. No updates gives
holdover after two seconds and UNSYNCED after ten. Monotonic time always drives
routing/session/queue deadlines.

Each peer can have a primary and a backup path (UDP endpoints or RF). Only a
fresh authenticated ping/pong exchange establishes path liveness. After two
seconds without replies the sender selects an available backup. Failover is
sticky to avoid flapping. Neither path health nor an RX-only signal is treated
as proof of bidirectional reachability.

For Ethernet tunneling, set `tap` to a new TAP name and `ethernet_peer` to a
remote node ID. Creating TAP requires the appropriate Linux capability. The
operator configures its address or bridge membership. Frames up to 1518 bytes
are fragmented with boot/sequence IDs and bounded reassembly (64 frames, one
second). Reflected recently injected Ethernet frames are suppressed for two
seconds. Do not assume this narrow reflection guard replaces network-wide
spanning tree in an arbitrary external L2 topology.

## Keys and trust

Peers use separate pairwise 32-byte provisioning keys, not MAC addresses.
Fresh random challenges derive directional session keys with HKDF; counters
supply unique AES-GCM nonces per direction and session. Headers are authenticated;
replay state advances only after authentication. This follows the nonce
requirements of [AES-GCM](https://cryptography.io/en/49.0.0/hazmat/primitives/aead/).
An old handshake replay after restart cannot answer the responder's new
challenge. Sessions rotate before one hour and stop at the counter limit.
There is no plaintext fallback.

Stage a new key ID at both endpoints, select it as `active_key` on the lower-ID
initiator, and call `reload_keys` at both endpoints. After reauthentication,
remove the old key and reload again to revoke it. At most two IDs are accepted
per peer. Reload failures leave the current configuration intact; intentional
reload clears forwarding state until peers authenticate again.

Encryption/authentication is **hop by hop**: relays are trusted to see payloads
and accurately forward origin claims. This PSK protocol does not provide
forward secrecy or end-to-end signatures against a compromised authorized
relay. Independent security review and the required deployment threat model
remain release gates.

## Validation

Recorded results are in [Software validation](reports/mission-software-validation.md).

```sh
ctest --test-dir build --output-on-failure
python3 tests/mission/test_mission.py -v
python3 tests/mission/test_service.py -v
python3 tests/mission/qualify.py --seconds 3600 --output /tmp/mission-soak.json
build/src/tools/sdr-modem-characterize 100 > /tmp/modem-awgn.csv
bash tests/qam64_test.sh "$PWD"
```

The process test uses real loopback UDP/Unix sockets and returns SKIP (77) if
the environment forbids sockets. The soak is accelerated simulated time, with
mobility/link loss, RF degradation, relay reboot and bounded-state assertions.
The characterization tool sweeps fixed modes and RS in deterministic
**symbol-domain AWGN**, outputting known-reference EVM, conditional raw BER,
PER and clipping. It does not simulate all RF impairments or replace an
attenuator/OTA campaign. RTL simulation cross-checks all 64 labels against the host liquid-dsp mapper
(with at most one fixed-point LSB of rounding error) and verifies decision
boundaries, backpressure, reset and clipping. No hardware tests, bitstream
promotion or production deployment are implied by these commands.
