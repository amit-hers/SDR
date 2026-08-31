# LibreSDR project sources (snapshot)

Copies of the four files that turn a stock ADI `projects/libre` checkout into
the hardware-verified modem build. The ADI tree itself is a clone and is not
version-controlled here, so without this snapshot the integration would have to
be rediscovered.

Copy them over `adi-hdl/projects/libre/` and run `make`.

| file | why it differs from stock |
|---|---|
| `system_constr.xdc` | the MEASURED AD9363 pin map (see `../constraints/libresdr_rev5_ad9363_cmos.xdc`); the earlier port swap is undone |
| `system_bd.tcl`     | sources `../bd/sdr_insert.tcl` at the end to splice in the modem |
| `system_project.tcl`| adds `sdr_clock_groups.xdc` as implementation-only |
| `system_top.v`      | stock |

Verified 2026-08-31 on hardware: timing WNS +0.145 ns, `axi_ad9361` PN monitor
locked with zero errors on all four channels, RX bytes delivered over DMA, and
`dac_dunf` clean on TX. Bring-up order and the DMA IRQ re-unmask are mandatory —
see the `modem-bringup-order` note.
