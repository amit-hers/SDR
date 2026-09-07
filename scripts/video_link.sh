#!/usr/bin/env bash
# video_link.sh -- send H.264 over a link and measure what actually arrives.
#
#   video_link.sh loopback [--bitrate K] [--secs N]        both ends locally
#   video_link.sh tx --host H [--port P] [--bitrate K]     transmit
#   video_link.sh rx [--port P] [--secs N] [--display]     receive and measure
#   video_link.sh budget                                   what the link can carry
#
# Plain RTP over UDP rather than RTSP. RTSP adds a TCP control channel that has
# to survive the same lossy link as the media, and for a one-way feed it buys
# nothing; the payload on the wire is identical either way. An RTSP server is
# still useful for LOCAL viewing, and `rx --display` gives you that.
#
# WHAT IS MEASURED, and why each is needed separately:
#   offered    what the encoder produced        -- did x264 honour the bitrate
#   delivered  what arrived and depayloaded     -- what the link actually carried
#   decoded    frames the decoder accepted      -- delivered bytes can still be
#                                                  useless if they are the wrong
#                                                  ones, and H.264 degrades by
#                                                  losing reference frames, not
#                                                  by arriving slowly
set -uo pipefail

PORT=5005
BITRATE=700          # kbps; see `budget`
SECS=15
SIZE=640x360
FPS=30
HOST=""
DISPLAY_OUT=0
OUT=/tmp/sdr-video

cmd="${1:-}"; shift || true
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)    HOST="$2"; shift 2 ;;
    --port)    PORT="$2"; shift 2 ;;
    --bitrate) BITRATE="$2"; shift 2 ;;
    --secs)    SECS="$2"; shift 2 ;;
    --size)    SIZE="$2"; shift 2 ;;
    --fps)     FPS="$2"; shift 2 ;;
    --display) DISPLAY_OUT=1; shift ;;
    --out)     OUT="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
W="${SIZE%x*}"; H="${SIZE#*x}"
mkdir -p "$OUT"

need() { command -v "$1" >/dev/null || { echo "missing: $1" >&2; exit 1; }; }

budget() {
cat <<'EOF'
Link capacity, and what fits.

  HOST DAEMON (libiio)          usable        fits
    bw 1 MHz QPSK               ~1.23 Mbit/s  360p30 @700k
    bw 2 MHz QPSK               ~2.46 Mbit/s  480p30 @1.2M
  FABRIC MODEM (PL)
    17.28 MS/s QPSK              7.85 Mbit/s  720p30 @4M

Usable = raw x (1200/1270 frame efficiency) x tx_duty_max.
Leave headroom: an encoder asked for exactly the link rate will overrun it on
every scene change, and the loss lands on reference frames.

Set the radio to match before streaming:
  sdrctl set bw_mhz 1 modulation QPSK
  sdrctl apply
EOF
}

# x264 tuned for a lossy one-way link:
#   tune=zerolatency  no B-frames, no lookahead -- latency, and B-frames widen
#                     the blast radius of any single lost packet
#   key-int-max       an IDR every second, so a decoder that loses reference
#                     frames recovers in <=1 s instead of staying broken
#   bitrate/vbv       cap the peak, not just the average: the link has no
#                     elasticity and a burst is simply dropped
ENC="x264enc tune=zerolatency speed-preset=veryfast bitrate=$BITRATE key-int-max=$FPS \
     vbv-buf-capacity=1000 pass=cbr ! video/x-h264,profile=baseline"

tx() {
  need gst-launch-1.0
  [[ -n "$HOST" ]] || { echo "tx needs --host" >&2; exit 2; }
  echo "TX -> $HOST:$PORT   ${SIZE}@${FPS}  ${BITRATE} kbps  for ${SECS}s"
  # tee to a file as well, so `offered` is measured rather than assumed.
  timeout "$SECS" gst-launch-1.0 -q \
    videotestsrc is-live=true pattern=smpte \
    ! video/x-raw,width=$W,height=$H,framerate=$FPS/1 \
    ! timeoverlay ! videoconvert ! $ENC \
    ! tee name=t \
      t. ! queue ! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au ! filesink location=$OUT/tx.h264 \
      t. ! queue ! rtph264pay config-interval=1 pt=96 ! udpsink host=$HOST port=$PORT sync=true
}

rx() {
  need gst-launch-1.0
  # Run longer than the transmitter so the receive window BRACKETS it. Sized
  # equal, the receiver stops while the sender is still going and the shortfall
  # is indistinguishable from link loss -- on loopback that read as 91%
  # delivered when nothing had actually been lost.
  local RXSECS=$((SECS + 5))
  echo "RX on udp/$PORT for ${RXSECS}s (transmitter runs ${SECS}s)"
  local sink="fakesink sync=false"
  [[ $DISPLAY_OUT -eq 1 ]] && sink="autovideosink sync=false"
  rm -f "$OUT/rx.h264"
  # latency=200: enough to absorb the link's jitter without hiding loss.
  timeout "$RXSECS" gst-launch-1.0 -q \
    udpsrc port=$PORT caps="application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96" \
    ! rtpjitterbuffer latency=200 \
    ! rtph264depay ! h264parse \
    ! tee name=t \
      t. ! queue ! video/x-h264,stream-format=byte-stream,alignment=au ! filesink location=$OUT/rx.h264 \
      t. ! queue ! avdec_h264 ! videoconvert ! fpsdisplaysink video-sink="$sink" \
           text-overlay=false signal-fps-measurements=true 2>&1 | \
    grep -oE 'average: [0-9.]+' | tail -1 | sed 's/^/  decoded fps /' || true
  report
}

report() {
  local rxb=0 txb=0
  [[ -f "$OUT/rx.h264" ]] && rxb=$(stat -c%s "$OUT/rx.h264")
  [[ -f "$OUT/tx.h264" ]] && txb=$(stat -c%s "$OUT/tx.h264")
  echo
  echo "  measurement window: ${SECS}s of transmission"
  printf '  delivered %d B = %.2f Mbit/s\n' "$rxb" \
         "$(python3 -c "print($rxb*8/$SECS/1e6)")"
  if [[ $txb -gt 0 ]]; then
    printf '  offered   %d B = %.2f Mbit/s\n' "$txb" \
           "$(python3 -c "print($txb*8/$SECS/1e6)")"
    printf '  delivered/offered = %.1f%%\n' \
           "$(python3 -c "print(100.0*$rxb/$txb if $txb else 0)")"
  fi
  # Decode the received stream on its own. Bytes arriving is not the same as
  # video surviving: H.264 degrades by losing reference frames, so a stream can
  # be 99% delivered and still unwatchable.
  #
  # Uses GStreamer, not ffmpeg: ffprobe is not installed here, and the snap
  # ffmpeg has a private /tmp so it cannot see these files at all -- it reported
  # "No such file or directory" on a file that was plainly there.
  if [[ -s "$OUT/rx.h264" ]]; then
    local frames err
    frames=$(python3 - "$OUT/rx.h264" <<'PYEOF'
import sys
# Count coded pictures by scanning Annex-B start codes for slice NAL types.
# nal_unit_type 1 (non-IDR) and 5 (IDR), taking only first-slice-in-picture,
# which for this encoder's single-slice output is one per frame.
d = open(sys.argv[1], 'rb').read()
n, i = 0, 0
while True:
    i = d.find(b'\x00\x00\x01', i)
    if i < 0 or i + 4 >= len(d): break
    t = d[i+3] & 0x1F
    # Count only the FIRST slice of each picture. x264 uses sliced threading, so
    # a frame is several NALs of type 1/5 and counting them all overstates the
    # frame rate ~5x. first_mb_in_slice is a ue(v) at the top of the slice
    # header; the value 0 encodes as a single '1' bit, so the first payload byte
    # has its MSB set exactly for a first slice.
    if t in (1, 5) and (d[i+4] & 0x80): n += 1
    i += 3
print(n)
PYEOF
)
    err=$(gst-launch-1.0 -q filesrc location="$OUT/rx.h264" ! h264parse ! avdec_h264 \
            ! fakesink sync=false 2>&1 | grep -ciE 'error|warning' || true)
    echo "  coded pictures in the received stream: ${frames:-0}"
    echo "  decoder complaints while replaying it: ${err:-0}"
    if [[ "${err:-0}" -gt 0 ]]; then
      echo "  (complaints mean lost reference frames -- visible as smearing until the next IDR)"
    fi
  fi
}

loopback() {
  need gst-launch-1.0
  echo "Loopback on 127.0.0.1:$PORT -- validates the pipeline, not the radio."
  HOST=127.0.0.1
  rx & local rxpid=$!
  sleep 2
  tx
  wait $rxpid 2>/dev/null
}

case "$cmd" in
  budget)   budget ;;
  tx)       tx ;;
  rx)       rx ;;
  loopback) loopback ;;
  *) echo "usage: $0 {loopback|tx|rx|budget} [options]"; budget; exit 2 ;;
esac
