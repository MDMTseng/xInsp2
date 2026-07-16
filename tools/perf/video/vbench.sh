#!/usr/bin/env bash
# vbench.sh — offline hardware-video-encode bench for image streaming.
# Sweeps Intel QSV (+ CPU x264 ref) over 5MP/20MP, MOVING vs STATIC content,
# reporting encode throughput (fps) and bitrate (KB/frame, Mbps). No browser.
set -u
FF="ffmpeg -hide_banner -loglevel error"
N=90            # frames per clip
FR=30           # nominal frame rate (for bitrate math)

# codecs to try (encoder-agnostic: on AMD swap qsv->amf, on ARM->the SoC MFT)
CODECS="h264_qsv hevc_qsv av1_qsv mjpeg_qsv libx264"

run_one () {   # size_name  W  H  content
  local nm=$1 W=$2 H=$3 content=$4
  local raw="src_${nm}_${content}.raw"
  # generate the raw RGB sequence
  if [ "$content" = "moving" ]; then
    $FF -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FR}" -frames:v $N -pix_fmt rgb24 -f rawvideo "$raw" 2>/dev/null
  else # static-ish: one testsrc2 frame held for N frames (tiny change) — best case for temporal codecs
    $FF -f lavfi -i "testsrc2=size=${W}x${H}:rate=1" -frames:v 1 -pix_fmt rgb24 -f rawvideo one.raw 2>/dev/null
    : > "$raw"; for i in $(seq 1 $N); do cat one.raw >> "$raw"; done
  fi
  local rawbytes; rawbytes=$(stat -c%s "$raw")
  # warm the OS cache (so disk read doesn't skew fps)
  cat "$raw" > /dev/null

  for c in $CODECS; do
    # per-codec output container ext (raw ES) so ffmpeg picks a muxer
    local ext=h264
    case "$c" in
      hevc_qsv)  ext=hevc ;;
      av1_qsv)   ext=ivf ;;
      mjpeg_qsv) ext=mjpeg ;;
    esac
    local out="out_${nm}_${content}_${c}.${ext}"
    # quality knobs: qsv uses -global_quality; x264 uses -crf; mjpeg uses -q:v
    local q="-global_quality 25"
    case "$c" in
      libx264)   q="-crf 23 -preset veryfast" ;;
      mjpeg_qsv) q="-global_quality 25" ;;
    esac
    local t0 t1 secs fps size kbpf mbps
    t0=$(date +%s.%N)
    if $FF -f rawvideo -pix_fmt rgb24 -s ${W}x${H} -r ${FR} -i "$raw" -c:v $c $q -y "$out" 2>err.txt; then
      t1=$(date +%s.%N)
      secs=$(awk "BEGIN{print $t1-$t0}")
      fps=$(awk "BEGIN{printf \"%.1f\", $N/$secs}")
      size=$(stat -c%s "$out")
      kbpf=$(awk "BEGIN{printf \"%.1f\", $size/$N/1024}")
      mbps=$(awk "BEGIN{printf \"%.1f\", $size*8*$FR/$N/1e6}")
      printf "  %-11s | %6s fps | %8s KB/frame | %7s Mbps\n" "$c" "$fps" "$kbpf" "$mbps"
      rm -f "$out"
    else
      printf "  %-11s | FAILED: %s\n" "$c" "$(tail -1 err.txt | cut -c1-70)"
    fi
  done
  rm -f "$raw" one.raw
}

echo "=================================================================="
echo " Offline HW video-encode bench (Intel QSV) — $N frames @ ${FR}fps"
echo " JPEG baseline (turbo, per doc 34): 5MP ~105 KB/frame, 20MP ~350 KB/frame"
echo "=================================================================="
for spec in "5MP 2448 2048" "20MP 5120 3840"; do
  set -- $spec
  for content in moving static; do
    echo ""
    echo "### $1 ($2x$3) — $content content ###"
    run_one "$1" "$2" "$3" "$content"
  done
done
echo ""
echo "done."
