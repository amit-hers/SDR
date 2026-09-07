# State protocol and `sdrctl`

Two things: a wire format for reporting what a radio is doing, and a command-line
tool for reading and changing it.

## The protocol

`StatePacket` (`include/sdr/telemetry/StatePacket.hpp`) is one datagram
describing a node. The same bytes are used in both places it is carried:

* over **UDP**, answered by the daemon's `TelemetryServer` — this is what
  `sdrctl status` speaks;
* over the **RF link** itself, as a frame with `FL_CTRL` set, where it is
  already inside a CRC-checked frame.

```
 offset  size  field
   0      4    MAGIC        0x53445253 "SDRS"
   4      2    version      currently 1
   6      2    body_len     bytes of body that follow
   8      N    body         fixed little-endian fields, see below
   8+N    4    CRC-32       over the header and body together
```

Body, in order: `node_id`, `uptime_s`, `mode`, `freq_tx_hz`, `freq_rx_hz`,
`bw_hz`, `sps`, `modulation`, `tx_atten_cdb`, `frames_tx`, `frames_rx_good`,
`frames_rx_bad`, `dropped`, `bytes_tx`, `bytes_rx`, `fec_corrected`,
`rssi_cdbm`, `snr_cdb`, `tx_kbps`, `rx_kbps`, `tx_duty_pct`, `temp_cc`,
`fpga_magic`, `fpga_version`, `fpga_abi`, `regmap_ver`.

Scaled integers rather than floats: `_cdb`/`_cdbm`/`_cc` are hundredths, so
25.00 dB and −63.12 dBm are exact and there is no float format to agree on.

### Three properties worth knowing

**It is written field by field, little-endian** — not a `memcpy` of a struct.
Struct padding and host endianness are not part of a wire format, and a report
that only decodes on the machine that produced it is not a protocol.

**`body_len` is what makes it extensible.** A newer sender may append fields; an
older receiver reads what it knows and ignores the rest. A newer receiver
reading an older sender's shorter body gets defaults for the missing tail, not
garbage. So fields are only ever **appended** — never reordered or resized — and
the version number rises only on a change that breaks that promise.

**A corrupt report is rejected, never rounded.** The CRC covers the header as
well as the body, so a corrupted length cannot verify. The test suite asserts
that *every single-bit flip anywhere in the packet* is rejected: a corrupt
report that decodes is worse than no report, because it is indistinguishable
from the truth.

### The server

```json
"telemetry_port": 5140,
"telemetry_bind": "127.0.0.1"
```

Bound to **loopback by default**. The report names the node, its frequencies and
its traffic volumes, and carries no authentication, so publishing it on a
routable address is a deliberate choice rather than a default. Set
`telemetry_port` to 0 to disable it.

It is request/response only and replies solely to the 4-byte request `STAT`;
anything else is ignored in silence, so it cannot be pointed at a third party as
a traffic amplifier. A bind failure is logged and ignored — telemetry is a
convenience and must never be able to stop the radio.

## `sdrctl`

```bash
sdrctl show                                  # the effective configuration
sdrctl get freq_tx_mhz
sdrctl set freq_tx_mhz 434 bw_mhz 1 modulation QPSK
sdrctl apply --node A                        # push to a RUNNING daemon
sdrctl status --host 127.0.0.1 [--json]      # live state over the wire
sdrctl net show
sdrctl net set --ip 192.168.2.17 --mask 255.255.255.0
```

### Editing configuration

Changes are **surgical**: the file is searched for one key, that value is
replaced, and everything else is copied through untouched. Re-serialising from a
struct would silently drop any field the tool does not model, and `config.json`
carries far more than the handful `sdrctl` exposes. The write goes to a
temporary and is renamed into place, so an interrupted write cannot leave a
truncated config behind.

Values are **validated against what the hardware and DSP accept**, because the
failure mode otherwise is a daemon that starts, runs, and quietly does the wrong
thing:

| key | accepted | why |
|---|---|---|
| `freq_tx_mhz`, `freq_rx_mhz` | 325–3800 | the AD936x tuning range; outside it the driver clamps silently |
| `bw_mhz` | 1, 2, 5, 10, 20 | above 2 the receive duty cycle collapses and *fewer* frames are delivered |
| `samples_per_symbol` | 2 or 4 | 4 is the validated mode |
| `tx_atten_db` | 0–89 | driver range |
| `tx_duty_max` | 0–1 | fraction of airtime |
| `tap_mtu` | 576–1386 | `MAX_PAYLOAD` 1400 less the 14-byte Ethernet header |
| `modulation` | BPSK, QPSK, 16QAM, 64QAM | |
| `mode` | bridge, mesh, p2p-tx, p2p-rx, scan | |

A multi-key `set` is **all-or-nothing**: every value is validated before any is
written, so a typo in the third setting cannot leave the first two applied.

### Applying to a running daemon

`sdrctl apply` writes the reload file and sends `SIGUSR2`, which is what the
daemon watches. Pass `--node NAME` when the dashboard launched the daemon, since
it gives each node its own path via `SDR_RELOAD_FILE`; without the flag the
default `/tmp/sdr_reload.json` is used. The daemon applies frequency and
attenuation live.

### Network settings

`sdrctl net` edits `config.txt` on the radio's USB mass-storage volume, found
automatically under `/media`, `/run/media` or `/mnt`.

**The board applies it only when the volume is ejected**, and does nothing at all
before that — so `net set` says so explicitly rather than implying it took
effect. Editing `/opt/config.txt` on the board instead does *not* persist: that
path is in the ramdisk. See
[Deployment](DEPLOYMENT.md#--persist-resets-the-boards-identity).
