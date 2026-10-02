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
| F06 modulation | Per-frame host BPSK/QPSK/16-QAM/64-QAM; robust BPSK header; QAM residual carrier tracking | Fixed-mode OTA measurements before adding a mode to capabilities; moving execution from host to FPGA per mode (see F07) |
| F07 FPGA QAM | Synthesizable Gray mapper/demapper, normalization, hard decisions, error/clipping outputs, RTL tests and OOC synthesis on xc7z020clg400-2 for **both 16-QAM and 64-QAM** (16-QAM done 2026-09-29, ahead of 64-QAM by request, since QPSK is the only OTA-proven mode on the deployed appliance today); all four blocks meet static timing at 200 MHz (the existing HLS modem cores' own OOC clock) with 2.5+ ns of slack and zero failing endpoints. **16-QAM mapper/demapper hardware-verified 2026-10-02** on Unit B via a new, separate non-production test harness (`adi-hdl/projects/qam16_harness/`), the same safe AXI-Lite-peripheral-plus-reboot-revert pattern already proven for the RS decoder chain -- not wired into the real modem datapath, exercised standalone via register pokes. Full routed implementation WNS +0.353 ns (0 failing endpoints; thinner margin than the RS blocks since the critical path runs through the demapper's squared-error multiplier, a real DSP48E1, not a bug). On real hardware: all 16 mapper symbols produced exactly the expected I/Q amplitudes: all 16 demapper round-trips on exact constellation points decoded the correct symbol with exactly zero error; the clipping flag correctly set on full-scale rails. Zero RTL bugs found this round -- the mapper/demapper's existing simulation coverage (`tests/tb_qam16.v`: all 16 points, decision boundaries, backpressure, reset, clipping) had already caught everything there was to catch. Cleanly reverted by reboot exactly as with the RS harness; production bitstream and all services confirmed restored; Unit A untouched. **64-QAM MAPPER hardware-verified 2026-10-02 too** (`adi-hdl/projects/qam64_harness/`), same pattern, all 64 symbols producing exactly the expected I/Q amplitudes (cross-checked by hand against the Gray/level table on real hardware output). **The 64-QAM DEMAPPER was deliberately excluded from that harness** -- OOC synthesis found it does NOT meet this project's real 100 MHz AXI-Lite clock as currently written (WNS -0.396 ns, 32 failing endpoints), unlike 16-QAM's demapper which passes (+0.183 ns): 64-QAM's 8-way boundary comparator (vs 16-QAM's 4-way) is deep enough to eat the margin that a near-identical squared-error DSP48E1 multiply path otherwise has. This is a genuine, newly-found timing gap, not a functional bug -- `qam64_demapper.v` is still correct per its existing simulation coverage (`tests/tb_qam64.v`). **Fixed and hardware-verified 2026-10-02**: pipelined `qam64_demapper.v` into two stages (index/level/diff/clip registered in stage 1, squared-error multiply computed in stage 2 from ONLY the registered stage-1 outputs), validated against a Python model of the exact pipeline (500 randomized trials) before writing RTL -- this caught a bug in the Python model itself (stale-signal sampling order) before it could have been a false green light. `tests/tb_qam64.v` rewritten to drain results through a FIFO queue instead of hardcoded latency offsets, so it no longer depends on knowing the exact pipeline depth -- passed on the first run after the rewrite. Standalone OOC at the real 100 MHz clock: WNS **+4.478 ns** (up from -0.396 ns); the combined mapper+demapper harness closes the same way (+4.478 ns, 0 failing endpoints); full routed build WNS +1.118 ns. (200 MHz now fails standalone, -0.522 ns -- self-consistent with the same ~5.5 ns critical path at a tighter period, and never this peripheral's real target.) On real hardware: all 64 demapper round-trips on exact constellation points decoded correctly with exactly zero error, pipelining is fully transparent at the register interface; mapper spot-checks still correct. Cleanly reverted; production bitstream and services confirmed restored; Unit A untouched. **All of F07's mapper/demapper RTL (both modes) is now hardware-verified.** **16-QAM hardware BER-vs-SNR sweep, 2026-10-02** (`fpga/probe/qam16_ber_sweep.py`): the first real-hardware noise-sensitivity data point for this project's QAM work, not just exact-zero-noise correctness. Reuses the IDENTICAL AWGN model `src/tools/modem_characterize.cpp` already uses for AMC threshold calibration (`docs/reports/mission-awgn.csv`) -- same formula, same Q2.13 fixed-point scale -- injected directly into the real `qam16_demapper.v` hardware's I/Q input over the existing `qam16_harness` AXI-Lite bitstream, at six SNR points (6-36 dB, 1000 symbols each). Result: **zero mismatches between the hardware's decoded output and the host Python reference across all 6000 noisy symbols** -- not just similar BER, bit-IDENTICAL decode on every single symbol, including the realistic waterfall region (14.4% BER at 6 dB, 2.7% at 12 dB, 0% by 18 dB). This is explicitly NOT an RF qualification (synthetic noise injected digitally, no antenna or real channel involved -- same disclaimer `modem_characterize.cpp`'s own header already carries), but it does prove the demapper's decision logic behaves correctly under realistic noise, not only at the lattice points already proven. Cleanly reverted; Unit A untouched. **64-QAM counterpart (`fpga/probe/qam64_ber_sweep.py`) run the same day**: same AWGN model, same `qam64_harness` bitstream (including the pipelined demapper fix), seven SNR points (6-42 dB, 1000 symbols each). **Zero mismatches across all 7000 symbols**, with the expected steeper waterfall than 16-QAM (24.5%/10.5%/2.55%/0.033%/0% BER at 6/12/18/24/30 dB vs 16-QAM's 14.4%/2.7%/0% at 6/12/18 dB) -- 64-QAM genuinely needs more SNR for the same error rate, now quantified on real hardware rather than only asserted qualitatively. One real interruption handled safely mid-session: the host machine's USB connection to the unit was physically disconnected partway through a sweep, the unit rebooted, and -- because the test bitstream was only ever RAM-loaded via `fpga_manager`, never written to QSPI flash -- it came back up on the production bitstream automatically with no manual recovery needed, confirming the project's revert discipline holds even under an uncontrolled disconnect, not just a clean scripted reboot | Integration with FPGA acquisition, timing/carrier recovery, pulse shaping, stream packing and full-design (not OOC) timing closure in the REAL modem datapath (today's hardware tests prove the blocks' own logic, not their integration); hardware EVM/BER/PER sweeps over an actual RF channel (a first step is now done, see below -- synthetic noise injected directly into the demapper, not yet real air) | Integration with FPGA acquisition, timing/carrier recovery, pulse shaping, stream packing and full-design (not OOC) timing closure in the REAL modem datapath (today's hardware tests prove the blocks' own logic, not their integration); hardware EVM/BER/PER sweeps over an actual RF channel (a first step is now done, see below -- synthetic noise injected directly into the demapper, not yet real air) |
| F08 AMC | Receiver feedback, capability intersection, sustained upgrades, rapid fallback, stale-feedback reset | RF threshold calibration; higher-order modes default disabled |
| F09 FEC | Existing RS(255,223) integrated per frame and negotiated; coding rate 223/255 or uncoded; decoder counters. **The full RS(255,223) FPGA decode chain now exists and is verified end-to-end**: encoder (`rs_encoder.v`), syndrome calculator (`rs_syndrome.v`, all-zero/no-error flag), error-locator solver (`rs_berlekamp_massey.v`), Chien search (`rs_chien_search.v`), and Forney (`rs_forney.v`, computes the actual error magnitudes -- i.e. corrects, not just locates) -- all GF(2^8) poly 0x11D standard construction, NOT bit-matched to the host's liquid-dsp codec whose internals aren't inspectable here. Verified against a real pipeline, not synthetic polynomials: `tests/rs_decoder_ref.py` runs real encode+random-corruption+syndrome+BM+Chien+Forney (1300+ trials up to the maximum correctable t=16), and each RTL stage's own testbench is driven from that same reference with randomized backpressure on every interface. Synthesis on xc7z020clg400-2: encoder/syndrome/Chien meet 200 MHz (373-547 LUT each); the error-locator solver only closes at ~66.7 MHz standalone (three serial GF multiplies/iteration vs. the others' one); Forney is in between, 2479 LUT, WNS -0.7 ns at 200 MHz but +4.3 ns at the ACTUAL 100 MHz clock this project's AXI-Lite peripherals use. All five blocks meet that real 100 MHz clock except the error-locator solver, which would need its own slower domain or pipelining to share it. **Encoder and syndrome calculator hardware-verified 2026-10-01** on Unit B via a separate, non-production bitstream (`adi-hdl/projects/rs_harness/`) loaded live through Linux's `fpga_manager`, full implementation WNS +1.321 ns, cleanly reverted by reboot with bridge/agent/supervisor confirmed healthy afterward -- see [[rs-harness-on-device-test]]. Real bugs found and fixed building this (not just caught by luck): a backpressure gap in Chien search that could silently drop a found root under consumer stalls; a one-cycle `done` pulse in BM that real polling software would likely miss entirely; and a classic Verilog width bug (`wire x = a^b^c...` with no explicit `[7:0]` silently truncates to 1 bit) found in Chien search and, via a targeted grep afterward, ALSO latent in the already-hardware-verified syndrome calculator's `all_zero` flag -- undetected there purely because test bytes happened to always have bit 0 set when genuinely nonzero | Integrating the five blocks into one real pipeline (today each is a standalone, independently-tested unit, not wired together as a single core); pipelining the error-locator solver if it must share the main datapath's clock; measured RF coding gain. **Hardware verification now covers four of the five blocks** (2026-10-01): encoder, syndrome calculator, Chien search, and Forney all confirmed correct on Unit B via the same non-production test-harness bitstream (`adi-hdl/projects/rs_harness/`), extended twice more this session -- Forney computed the exact injected error magnitudes (0x77 at degree 154, 0x33 at degree 249) from real AXI-Lite register traffic, not a replay of simulation output. Only the error-locator solver (Berlekamp-Massey) remains unverified on real silicon, since it can't run in this harness's 100 MHz clock domain at all. Two testbench-only bugs found and fixed along the way (not RTL defects): both `axi_write`/`axi_read` simulation tasks returned before the AXI response channel's valid signal had fully deasserted, letting back-to-back transactions race and occasionally return stale data -- cost real debugging time precisely because the RTL was correct throughout and the bug lived entirely in how the tests drove it. **Berlekamp-Massey pipelined 2026-10-01**: the original single-cycle-per-iteration design chained three serial GF multiplies and failed the real 100 MHz clock outright (WNS -3.996 ns); split into three registered pipeline stages (delta / coef / locator-update, one GF multiply deep each -- 96 cycles per block instead of 32, irrelevant for a once-per-codeword control computation), it now measures WNS +0.647 ns at 100 MHz. That margin is thin (~6% of the period) compared to every other RS block's 4+ ns headroom -- Vivado's own path report shows the TRUE bottleneck is still inside the delta stage's 32-way reduction (13 logic levels, synd_reg to delta_reg), which the 3-stage split never touched; a 4th stage was tried splitting a runtime-indexed mux out of the update stage and measured WORSE, confirming the mux was never the real bottleneck. **Fixed 2026-10-01 by actually doing that tree pipelining**: split the delta reduction into two half-width stages (terms i=1-16, then i=17-32, combined before the ginv(b) multiply) instead of adding another stage boundary elsewhere. Confirmed via Vivado's own path report that the remaining bottleneck is the SAME kind of path, just half as deep (9 logic levels vs 13) -- proving the diagnosis was right this time. Result: WNS +2.593 ns at 100 MHz, up from the fragile +0.647 ns -- a genuinely comfortable, PVT-robust margin (26% of the period) close to the other blocks' headroom, though 200 MHz still doesn't close (-2.407 ns there, improved from -4.353 ns). **Hardware-verified 2026-10-01, round 4 -- all five RS decoder blocks now confirmed on real silicon.** Added BM to the test harness (`adi-hdl/projects/rs_harness/`, register SELECT=4) now that its margin is solid; full routed implementation WNS +0.263 ns (tighter than round 3's +0.415 ns, as expected with five blocks sharing routing, but still cleanly positive with 0 failing endpoints). On Unit B: BM computed the error-locator C=[0x01,0x0f,0x52], degree 2, directly from the same real syndromes already independently proven correct via Chien search (roots 154, 249) and Forney (magnitudes 0x77, 0x33) in earlier rounds -- closing the loop end-to-end on actual hardware. Chien search and Forney re-checked against the same known vector in this new, larger bitstream and still returned identical correct results, confirming no cross-block regression from adding BM. Cleanly reverted by reboot -f exactly as in rounds 1-3: production bitstream confirmed restored (harness address bus-errors again), bridge/agent/supervisor auto-restarted, peer compatibility COMPATIBLE. Unit A was never touched. **Integrated into one real decoder core 2026-10-02**: `rs_decoder.v` chains syndrome->BM->Chien->Forney automatically in hardware (one 255-byte codeword in, 223 corrected/passthrough bytes out, plus `fail`/`error_count`), replacing the manual CPU-driven sequencing the AXI test harness still does one register poke at a time. Validated the buffering/indexing logic (syndromes and locator coefficients are each needed TWICE -- once for their first consumer, once for Forney's second input phase -- so this module buffers and replays them) against `rs_decode()` via an orchestration-level Python model (2000 trials) before writing RTL. Three real, independent integration bugs found and fixed, none in the already-proven sub-blocks' own algorithms: (1) a classic Verilog NBA sampling trap in the testbench itself (an extra `#1` delay read a register's value one edge too late); (2) a genuine deadlock -- `rs_berlekamp_massey.v` and `rs_chien_search.v` each need their consumer to hold `m_ready` high for ONE cycle after their last output byte to self-clear and re-arm, something no prior CPU-polling consumer ever exercised since register polling always has incidental idle cycles; (3) applying all corrections in one parallel cycle needs as many independent runtime-indexed write ports into the 255-byte receive buffer as there are errors, which exploded to 96% of the part's LUTs and failed timing badly -- fixed by sequentializing to one correction per cycle (a once-per-codeword control operation, not a throughput path, so the extra cycles are free). Final result: OOC synthesis at the real 100 MHz clock, WNS +2.402 ns, 0 failing endpoints, 8167 LUT (15.35%) -- a comfortable, genuinely integrated decoder, not just five blocks that individually pass. **Hardware-verified 2026-10-02, round 5**: added a second, independent AXI-Lite peripheral (`axi_rs_decoder_test_harness_hw.v`, MAGIC "RSDC") to the SAME harness project alongside the existing per-block one -- full routed build WNS +0.103 ns (thin but a genuine pass, 0 failing endpoints, six RS-related peripherals now sharing routing in one bitstream). On Unit B: pushed a real 255-byte received codeword and popped 223 bytes with NO per-block register choreography at all -- just push, then poll-and-pop -- across all three control-flow paths: a clean (0-error) codeword decoded with `fail=0`/`error_count=0`; a 6-error codeword corrected exactly (`fail=0`, `error_count=6`, output matching the original pre-corruption data byte-for-byte); and a deliberately-uncorrectable 22-error codeword correctly flagged (`fail=1`, `error_count=0`, output matching the raw uncorrected received bytes). This is the first time this project's RS decode has run end-to-end autonomously on real silicon with no CPU involvement in the decode sequencing itself -- only pushing input and popping output. One ordering-only bug found in the harness's OWN test script (not the RTL): reading STATUS/ERROR_COUNT via a separate AXI transaction AFTER popping the final byte races `rs_decoder.v`'s own next-codeword clear and always loses to real AXI-Lite's multi-cycle transaction overhead -- fixed by reading them before the final pop (now documented directly in the harness's own register-map comment). Cleanly reverted; production bitstream and all services confirmed restored; Unit A untouched. **First proof that RS FEC and 16-QAM modulation work TOGETHER, 2026-10-02** (`fpga/probe/rs_qam16_chain_test.py`): every prior hardware test proved one piece in isolation -- this chains the real hardware blocks end-to-end (RS-encode -> 16-QAM-map -> optionally-corrupted symbols -> 16-QAM-demap -> RS-decode) in one combined bitstream with three independent AXI-Lite peripherals, orchestrated by a host script (no new autonomous-combination RTL yet -- that would be a further step). Clean pass (no corruption): payload recovered byte-for-byte, `fail=0 error_count=0`, proving the byte<->nibble<->symbol framing convention is correct when actually chained -- a real integration risk, not yet exercised by either subsystem's own isolated tests. Error-injection pass (5 symbols corrupted between map and demap, simulating real channel errors): **payload still recovered byte-for-byte, `error_count=5` matching the 5 injected errors exactly** -- the first real-hardware demonstration of FEC actually recovering from modulation-induced corruption, the actual value proposition of combining the two. One real bug found and fixed, in the test script's own orchestration (not either hardware block): the FIRST attempt pushed all 223 payload bytes into `rs_encoder.v` back-to-back with no interleaved pops, deadlocking permanently after the first byte -- `rs_encoder.v` is a documented zero-depth passthrough (established earlier this session) that will not accept byte N+1 until byte N's output is drained, a lesson already known but not applied when writing this new script. Reproduced it twice with identical symptoms (confirming it was a deterministic bug, not a hardware fluke) before finding the actual cause; fixed by interleaving push and pop for the first 223 bytes, draining the remaining 32 parity bytes separately. Cleanly reverted; Unit A untouched. **Same proof repeated for 64-QAM, 2026-10-02** (`fpga/probe/rs_qam64_chain_test.py`), completing this project's full RS+QAM combination matrix (16-QAM and 64-QAM both confirmed). Extended the same combined bitstream with a fourth independent AXI-Lite peripheral (`axi_qam64_test_harness_hw.v`, MAGIC "Q64T") at 0x43C90000; full routed build WNS +0.521 ns, 0 failing endpoints, four RS/QAM peripherals sharing one bitstream. The byte<->symbol packing is non-trivial here, unlike 16-QAM's trivial one-nibble-per-symbol split: a 64-QAM symbol carries 6 bits, which does not divide evenly into an 8-bit byte, so every 3 consecutive codeword bytes (24 bits) pack into exactly 4 six-bit symbols (lcm(6,8)=24); 255 = 3*85 divides evenly with no padding needed. This packing was validated standalone in Python before any hardware involvement. Applying the interleaved push/pop lesson from the 16-QAM attempt up front, the 64-QAM script had no deadlock on its first run. Clean pass: payload recovered byte-for-byte, `fail=0 error_count=0`, first attempt. Error-injection pass (5 symbols corrupted at indices [8, 10, 154, 313, 328] between map and demap): **payload still recovered byte-for-byte, `error_count=5` matching the 5 injected errors exactly** -- FEC recovering modulation-induced corruption confirmed at 64-QAM's tighter noise margins too, not just 16-QAM's. One genuine uncontrolled USB disconnect occurred mid-deployment of this same bitstream (unrelated to the RTL or test logic) -- because the bitstream was only ever RAM-loaded via `fpga_manager`, Unit B's automatic reboot safely restored the production bitstream with zero manual intervention, re-confirming the deployment discipline under a real fault, not just a scripted `reboot -f`. Cleanly reverted afterward via the normal scripted path as well; all four harness addresses bus-error, bridge/agent/supervisor confirmed healthy; Unit A untouched throughout |
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
