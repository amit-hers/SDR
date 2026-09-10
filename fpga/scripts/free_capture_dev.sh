#!/bin/sh
# Release the IIO char devices after an aborted run. Run ON the Pluto.
#
# The devices are SINGLE-OPEN. A drain or a feed left behind by an interrupted
# run holds one, and every later access then fails with EBUSY -- which reads
# like a dead radio and sent one debugging session chasing the RF path.
#
# Four things make this harder than `pkill`, and all four were met in practice:
#
#  * Killing the local ssh client does NOT stop what it launched on the board.
#    An interrupted acquisition loop keeps running under init and respawns the
#    drain as fast as it is killed, so the SCRIPT must die before its children.
#  * A drain blocked inside the driver's read does not die on SIGKILL at all.
#    At PKT_BYTES 32768 a read waits ~30 ms for a whole packet, and the process
#    sits in the kernel where the signal cannot reach it. Dropping
#    buffer/enable is what actually returns it.
#  * Leaving the core undrained stalls it: a stalled HLS core stops reading
#    AXI-Lite, so soft_reset is silently ignored afterwards. If reads still do
#    not come back, reload the PL and re-run rx_framed.sh -- that is the
#    reliable recovery, not more signals.
#  * A holder can be UNRECOVERABLE. If the fabric never consumed a submitted
#    DMA transfer, `echo 0 > buffer/enable` blocks in iio_dma_buffer_disable
#    waiting for it to retire, forever and in D state. Everything queues behind
#    that, including this script. Only a power cycle clears it -- so DETECT it
#    and say so rather than adding one more unkillable waiter.
set -u
. "$(dirname "$0")/iio_lookup.sh"
R=$IIO_RX

# ---------------------------------------------------------------------------
# Report any holder that is already unrecoverable, BEFORE touching anything.
# ---------------------------------------------------------------------------
wedged=0
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    st=$(awk '/^State:/{print $2}' "$p/status" 2>/dev/null)
    [ "$st" = "D" ] || continue
    wc=$(cat "$p/wchan" 2>/dev/null)
    case "$wc" in
        *iio*|*enable_store*|*dma_buffer*)
            echo "WEDGED: pid=$pid state=D wchan=$wc"
            echo "        cmd: $(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)"
            wedged=1;;
    esac
done
if [ "$wedged" = 1 ]; then
    echo "A holder is stuck in an uninterruptible kernel wait. SIGKILL cannot"
    echo "reach it and writing buffer/enable will block this script too."
    echo "POWER CYCLE THE BOARD -- nothing in software clears this state."
    exit 2
fi

# ---------------------------------------------------------------------------
# Kill live holders. Match BOTH directions and BOTH argv spellings: busybox ps
# renders `dd if=/x of=/y` without the '=' on some builds, so keying on the
# '=' misses the very processes this script exists to clear. Anchor on the
# device path instead, which is present either way.
# ---------------------------------------------------------------------------
for pat in '[a]cqframes.sh' '[r]xframes.sh' '[t]x_feed.sh'; do
    for p in $(ps w 2>/dev/null | grep "$pat" | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
done
sleep 1
# Anything with an iio char device on its command line, reader or feeder.
for p in $(ps w 2>/dev/null | grep '[i]io:device' | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
# A feed can also be `cat <file> > <chardev>`, where the redirect target never
# appears in argv at all.
for p in $(ps w 2>/dev/null | grep '[c]at /tmp/' | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
sleep 1

# ---------------------------------------------------------------------------
# Dropping buffer/enable is what actually returns a process blocked inside the
# driver; SIGKILL cannot reach it there. Do it for BOTH directions -- but in a
# background subshell with a bounded wait, because this write is exactly the
# one that hangs when a transfer never retired. Hanging here would leave the
# caller with no output and no diagnosis, which is how this cost a session.
# ---------------------------------------------------------------------------
disable_bounded() {
    dev=$1; name=$2
    ( echo 0 > "$dev/buffer/enable" 2>/dev/null ) &
    bg=$!
    n=0
    while [ $n -lt 10 ]; do
        kill -0 "$bg" 2>/dev/null || { wait "$bg" 2>/dev/null; return 0; }
        sleep 1; n=$((n+1))
    done
    echo "WEDGED: writing $name buffer/enable did not return after 10 s"
    echo "        (iio_dma_buffer_disable is waiting on a transfer the fabric"
    echo "         never consumed). POWER CYCLE THE BOARD."
    return 2
}
rc=0
disable_bounded "$R"       "rx" || rc=2
disable_bounded "$IIO_TX"  "tx" || rc=2
[ "$rc" = 2 ] && exit 2
sleep 1

echo "holders left:     $(ps w 2>/dev/null | grep -c '[i]io:device')"
echo "feeders left:     $(ps w 2>/dev/null | grep -c '[c]at /tmp/')"
echo "rx buffer/enable: $(cat "$R/buffer/enable" 2>/dev/null)"
echo "tx buffer/enable: $(cat "$IIO_TX/buffer/enable" 2>/dev/null)"
echo "NOTE: re-run rx_framed.sh / tx_fabric.sh afterwards to re-enable the"
echo "      buffers. If reads still return 0, reload the PL -- the modem core is"
echo "      stalled and cannot see a soft reset."
