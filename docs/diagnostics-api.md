# Authenticated diagnostics API

`sdr-diagnostics-api` is the appliance's read-only HTTP diagnostics surface.
It is intended to replace routine SSH inspection; it does not expose a shell,
configuration writes, resets, or arbitrary file access.

## Endpoints

All endpoints require `Authorization: Bearer <token>`.

| Endpoint | Source |
|---|---|
| `GET /api/v1/health` | API process health |
| `GET /api/v1/status` | `/mnt/jffs2/appliance_status.sh` |
| `GET /api/v1/metrics` | `/tmp/bridge_stats.json` |
| `GET /metrics` | Alias of `/api/v1/metrics` |
| `GET /api/v1/events` | Bounded selection of important appliance events |
| `GET /api/v1/logs` | Last 64 KiB of the appliance log |
| `GET /api/v1/diagnostic-bundle` | Status, metrics, events, and recent log in one bounded JSON document |

Unknown routes return 404. Any method other than GET returns 405. Missing or
incorrect authentication returns 401.

## Security contract

- Bind to the USB management address, never `0.0.0.0`.
- Generate a unique random token per unit.
- Store the token in a regular file with mode 0600. The service refuses to
  start if group or other permissions are present.
- HTTP is plaintext. Treat the USB management network as trusted and isolated;
  do not route this port onto the RF/user Ethernet or the public Internet.
- The status collector is executed directly with `exec`, never through a
  shell. Request parameters cannot select commands or paths.
- Requests, command runtime, status output, metrics, logs, events, and socket
  waits all have fixed bounds.

Structured events include `timestamp_monotonic_s`, `severity`, `subsystem`,
`code`, `node_id`, `peer_id`, and the original bounded message. The service
currently normalizes peer, recovery, preflight, configuration, supervisor, and
hang events from the existing appliance log. New producers should emit stable
event codes directly rather than requiring further text classification.

Example:

```sh
TOKEN=$(cat diagnostics.token)
curl -H "Authorization: Bearer $TOKEN" \
  http://192.168.2.1:8088/api/v1/status
```

## Deployment state

The stripped static ARMv7 binary is approximately 810 KiB. It does not fit
safely in the current JFFS2 partition, which has about 288 KiB free after the
bridge installation. Production persistence must therefore place the binary
in the next rootfs FIT and start it after the management interface is ready.
Do not consume JFFS2 emergency space to install it.
