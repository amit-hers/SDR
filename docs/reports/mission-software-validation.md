# Mission network software validation

Recorded 2026-09-26. These results cover the experimental implementation in
[Mission network](../mission-network.md). They do not qualify a radio, FPGA
bitstream, navigation system, or field deployment.

## Checks performed

- The complete registered CTest suite passed: 31/31 test entries. Tests that
  need local UDP/HTTP sockets were run with loopback networking available.
  The existing TAP/network-namespace harness has its own privilege checks;
  its CTest success is not evidence of a physical Ethernet/RF test.
- After additional decoder and adapter failure-path changes, all 11 affected
  CTest entries passed, including 26 deterministic mission cases and two real
  process tests. Those tests exercise an A–B–C topology without a direct A–C
  path, relay termination/restart, live key rotation, private management,
  adapter acknowledgement loss, and adapter identity mismatch.
- AddressSanitizer and UndefinedBehaviorSanitizer passed for the framing and
  split-modulation suites, including maximum-size RS codewords, correctable
  and uncorrectable errors, and QAM residual carrier tracking. Leak checking
  was disabled for this targeted sanitizer run.
- The installed executables were smoke-tested from a separate temporary
  installation prefix. Three generated private configurations passed strict
  validation through the installed launcher.
- Documentation links, Python compilation and `git diff --check` passed.

## One-hour accelerated simulation

Command:

```sh
python3 tests/mission/qualify.py --seconds 3600 --output /tmp/mission-soak.json
```

| Measurement | Result |
|---|---:|
| Simulated duration | 3600 seconds |
| Nodes | 4 |
| Offered packets | 36,000 |
| Delivered packets | 35,973 |
| Delivery fraction, including fault intervals | 99.925% |
| Duplicate deliveries | 0 |
| Largest observed queued message count per node | 1 |

The harness induced link loss/mobility, RF-degradation observations and relay
reboot. It checked route recovery, continuing stimulus/reception, and bounded
state. Its predeclared overall delivery floor was 90%, including fault
intervals. Simulated delivery is not a promise of equivalent RF performance;
this experiment is not a one-hour wall-clock hardware soak.

## Modem and RTL characterization

[mission-awgn.csv](mission-awgn.csv) records 100 frames at each of 48 combinations:
BPSK, QPSK, fixed 64-QAM, fixed 16-QAM; uncoded and RS(255,223); SNR values
6, 12, 18, 24, 30 and 36 dB. Seed: 640016. Every 36 dB point delivered all frames
byte-exact. The tool writes known-reference EVM, conditional raw BER, PER and
clipping fraction. Raw BER is conditional on acquisition producing a complete
frame; it must not be interpreted as an unconditional BER including lost frames.

```sh
build/src/tools/sdr-modem-characterize 100 > docs/reports/mission-awgn.csv
```

The model is symbol-domain additive white Gaussian noise. It does not include
all carrier, timing, analog, attenuation, amplifier or USB/DMA impairments.
Measured RF attenuation curves remain outstanding.

The RTL mapper/demapper simulation covers all 64 labels, exact/adjacent decision
boundaries, backpressure, reset and rail clipping. CTest also compares mapper
outputs with liquid-dsp's host 64-QAM constellation, allowing one fixed-point
LSB of rounding difference. The selected design is coherent, non-differential
64-QAM with signed 16-bit samples and 13 fractional bits. Acquisition, carrier
recovery, timing, pulse shaping and FPGA byte-stream integration remain outside
these RTL symbol blocks.

## Defects corrected during validation

- RS(255,223) expands a 1514-byte payload to 1785 bytes. Decoder buffers now use
  the complete rounded codeword bound instead of assuming 255 bytes of slack.
- A failed RS decode is rejected even if the incoming codeword's outer CRC is
  valid. Re-encoding verifies decoded data before delivery, and failed/rescued
  frames are counted separately.
- The host PHY selects a QAM-specific residual tracker for QAM payloads instead
  of applying the QPSK Costas detector to inner constellation points. Generic
  payload-tap callers retain their original callback behavior.
- Channel acquisition evidence is invalidated when an adapter restarts on its
  home channel. Physical acknowledgements include node identity; a mismatch
  disables forwarding. Monotonic deadlines bound rollback despite UTC steps.
- Queue overflow is returned as failed local admission. Transport send failure
  is counted as a forwarding failure, not successful forwarding.

## Remaining release gates

The per-feature table in [Mission network](../mission-network.md) identifies
remaining work. In particular: integrate the FPGA 64-QAM symbol blocks into a
complete modem, close synthesis/timing and hardware verification, characterize
fixed 64-QAM and then fixed 16-QAM versus attenuation, validate receiver metrics
and automatic switching on radios, test at least three physical mesh nodes,
qualify real redundant Ethernet paths, and run the field/long-duration matrix.
Independent security review is required before relying on the experimental
pairwise session protocol in a safety-critical deployment.

No production bitstream was flashed and no appliance deployment was promoted.
