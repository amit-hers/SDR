# Layer 3: IP over the radio

`mesh` mode carries **IP packets** over a TUN interface, as against `bridge`
mode, which carries Ethernet frames over a TAP. Use it when the radio is a
routed hop rather than a virtual cable — which is the normal case when a host
reaches the radio over its own Ethernet port and simply wants traffic to go
somewhere.

The daemon configures the interface itself. There is nothing to set up by hand.

## Configuration

```json
{
  "mode": "mesh",
  "tap_iface": "sdrl3",
  "ip_local":  "172.30.99.1",
  "ip_peer":   "172.30.99.2",
  "ip_prefix": 30,
  "route_via_peer": "192.168.50.0/24"
}
```

| key | meaning |
|---|---|
| `ip_local` | this node's address on the radio hop. Empty leaves the interface unconfigured, the old behaviour |
| `ip_peer` | the far node. Non-empty makes it a **point-to-point** link, which is what a radio hop is, and gives a host route to the far end for free |
| `ip_prefix` | `30` is two usable hosts: exactly one hop |
| `route_via_peer` | a network *behind* the far node, routed over the radio. Empty means host-to-host only |

Started, it reports what it did:

```
[sdr] sdrl3: 172.30.99.1/30 peer 172.30.99.2 mtu 1400
[sdr] route 192.168.50.0/24 via radio: up
```

and the interface really is configured:

```
inet 172.30.99.1 peer 172.30.99.2/32 scope global sdrl3
mtu 1400
172.30.99.2      dev sdrl3
192.168.50.0/24  via 172.30.99.2 dev sdrl3
```

## Why the daemon does this rather than a setup script

Two failures that a script cannot prevent and cannot even see, both met here.

**An address applied from outside gets flushed.** NetworkManager manages new
interfaces by default and will remove an address it did not assign, usually
within a second or two of the link appearing. Afterwards packets for the peer
match no specific route and leave by the **default** route instead — silently,
by the wrong interface, looking exactly like a radio that is not transmitting.
The daemon now asks NetworkManager to leave the interface alone, then **verifies
the address is still there** and refuses to continue if it is not:

```
mesh: sdrl3 lost its address immediately after it was set (now 'none').
      Something on this host is managing the interface -- normally
      NetworkManager. Mark it unmanaged and retry:
        nmcli device set sdrl3 managed no
```

**A link subnet that overlaps an existing route steals the traffic.** Same
symptom, different cause. Checked before anything is configured:

```
mesh: link subnet 192.168.2.50/24 overlaps a route this host already has.
      Traffic for the peer would leave by that route instead of the radio,
      with nothing reported. Choose a subnet that does not overlap
      (172.31.x is usually free where 10.x and 192.168.x are not).
```

The default route is deliberately **not** treated as a conflict — everything
overlaps it, so doing so would refuse every subnet.

## MTU

A TUN carries IP packets, so the 14 bytes `tap_mtu` reserves for an Ethernet
header are not spent. `mesh` uses `MAX_PAYLOAD` (1400) where `bridge` uses 1386.
Setting `tap_mtu` explicitly overrides this.

## Routing a LAN over the radio

To pass traffic from a local network out over the radio, set `route_via_peer` on
each node to the *other* side's network and enable forwarding:

```bash
sudo sysctl -w net.ipv4.ip_forward=1
```

Node A (LAN 192.168.40.0/24) and node B (LAN 192.168.50.0/24):

| | `ip_local` | `ip_peer` | `route_via_peer` |
|---|---|---|---|
| A | 172.30.99.1 | 172.30.99.2 | 192.168.50.0/24 |
| B | 172.30.99.2 | 172.30.99.1 | 192.168.40.0/24 |

Hosts on each LAN need a route to the other via their local node, or the node
needs to be their default gateway.

## Capacity

The radio is the constraint, not the interface. See
[Fabric modem](fabric-modem.md) and `scripts/video_link.sh budget`: the
host-daemon path yields ~1.23 Mbit/s at 1 MHz QPSK. Size what you send to it.
