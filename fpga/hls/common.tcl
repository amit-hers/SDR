# Shared settings for every HLS core in this tree.
#
# Sourced by each core's hls_build.tcl so the part, clock and export metadata
# are defined once. Override from the environment rather than editing:
#
#   SDR_HLS_PART   target device   (default xc7z020clg400-2, the Pluto+)
#   SDR_HLS_CLOCK  clock period ns (default 5, i.e. 200 MHz)
#
# The base ADALM-Pluto is an xc7z010clg400-1; the HamGeek Pluto+ this project
# runs on is an xc7z020clg400-2. They are not interchangeable -- a bitstream
# built for the wrong one will not load.

set part  [expr {[info exists ::env(SDR_HLS_PART)]  ? $::env(SDR_HLS_PART)  : "xc7z020clg400-2"}]
set clock_ns [expr {[info exists ::env(SDR_HLS_CLOCK)] ? $::env(SDR_HLS_CLOCK) : 5}]
set hls_vendor  "sdr-link"
set hls_library "dsp"

# Applies the settings a solution needs, in the order this toolchain wants
# them.
#
# No `-flow_target vivado` here: 2026.1 reports it deprecated -- "for 'vivado'
# value you may simply remove the option for equivalent behavior" -- and
# packaging for the Vivado IP catalog is now the default. Passing it produces
# two warnings on every core for no change in behaviour.
proc sdr_solution {name} {
    global part clock_ns
    open_solution -reset $name
    set_part      $part
    create_clock  -period $clock_ns -name default

    # No `config_interface -trim_dangling_ports`. It was a vivado_hls option and
    # 2026.1 rejects it outright:
    #   ERROR: [HLS 200-101] config_interface: Unknown option
    # Guarding it with `catch` does not help -- HLS records the error and fails
    # the run regardless of the Tcl result -- so it is simply gone. Trimming
    # unused top-level ports is default behaviour now.
}

# export_design arguments differ enough between versions to be worth one place.
proc sdr_export {description version} {
    global hls_vendor hls_library
    export_design -format ip_catalog \
        -description $description \
        -vendor  $hls_vendor \
        -library $hls_library \
        -version $version
}
