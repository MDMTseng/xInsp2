#!/usr/bin/env bash
# vcpu.sh — CPU cost of each encoder via ffmpeg -benchmark (utime+stime)/rtime
# = CPU cores busy. HW (QSV) should be near-zero CPU (work on the iGPU); CPU
# encoders (x264) and even turbo-JPEG burn cores. Includes the rgb->nv12 convert
# + I/O that a real pipeline also pays.
set -u
FF="ffmpeg -hide_banner -loglevel info -benchmark"
N=90; FR=30

measure () {  # codec  W  H  raw  ext  q
  local c=$1 W=$2 H=$3 raw=$4 ext=$5; shift 5; local q="$*"
  local log; log=$($FF -f rawvideo -pix_fmt rgb24 -s ${W}x${H} -r ${FR} -i "$raw" \
        -c:v $c $q -y "o.${ext}" 2>&1 >/dev/null; :)
  # ffmpeg prints: bench: utime=..s stime=..s rtime=..s
  local u s r cores fps
  u=$(echo "$log" | grep -oE "utime=[0-9.]+" | tail -1 | cut -d= -f2)
  s=$(echo "$log" | grep -oE "stime=[0-9.]+" | tail -1 | cut -d= -f2)
  r=$(echo "$log" | grep -oE "rtime=[0-9.]+" | tail -1 | cut -d= -f2)
  if [ -n "${r:-}" ] && [ "$r" != "0" ]; then
    cores=$(awk "BEGIN{printf \"%.2f\", ($u+$s)/$r}")
    fps=$(awk "BEGIN{printf \"%.0f\", $N/$r}")
    printf "  %-11s | %5s fps | CPU %5s cores (u=%ss s=%ss wall=%ss)\n" "$c" "$fps" "$cores" "$u" "$s" "$r"
  else
    printf "  %-11s | (no benchmark data / failed)\n" "$c"
  fi
  rm -f "o.${ext}"
}

FF2="ffmpeg -hide_banner -loglevel error"
echo "5MP moving raw..."; $FF2 -f lavfi -i "testsrc2=size=2448x2048:rate=$FR" -frames:v $N -pix_fmt rgb24 -f rawvideo r5.raw 2>/dev/null; cat r5.raw>/dev/null
echo "### 5MP (2448x2048) moving — CPU cost ###"
measure h264_qsv  2448 2048 r5.raw h264  -global_quality 25
measure hevc_qsv  2448 2048 r5.raw hevc  -global_quality 25
measure mjpeg_qsv 2448 2048 r5.raw mjpeg -global_quality 25
measure libx264   2448 2048 r5.raw h264  -crf 23 -preset veryfast
rm -f r5.raw

echo "20MP moving raw..."; $FF2 -f lavfi -i "testsrc2=size=5120x3840:rate=$FR" -frames:v $N -pix_fmt rgb24 -f rawvideo r20.raw 2>/dev/null; cat r20.raw>/dev/null
echo "### 20MP (5120x3840) moving — CPU cost ###"
measure hevc_qsv  5120 3840 r20.raw hevc  -global_quality 25
measure mjpeg_qsv 5120 3840 r20.raw mjpeg -global_quality 25
measure libx264   5120 3840 r20.raw h264  -crf 23 -preset veryfast
rm -f r20.raw
echo "done. (total logical CPUs: $(nproc))"
