#!/usr/bin/env bash
# tiling.sh — 20MP: single HEVC stream vs 4x H.264 tiles (2560x1920).
# Answers: (1) does tiling enable H.264 for 20MP? (2) do 4 concurrent QSV
# sessions parallelize on the single iGPU encoder, or serialize?
set -u
FF="ffmpeg -hide_banner -loglevel error"
N=60; FR=30
W=5120; H=3840; TW=2560; TH=1920
RAW=s20.raw

echo "gen 20MP $N-frame shapes..."; python gen_shapes.py $W $H $N $RAW; cat $RAW>/dev/null
sz(){ stat -c%s "$1"; }
kbpf(){ awk "BEGIN{printf \"%.1f\", $1/$N/1024}"; }
now(){ date +%s.%N; }
el(){ awk "BEGIN{printf \"%.2f\", $2-$1}"; }
fps(){ awk "BEGIN{printf \"%.1f\", $N/$1}"; }

echo ""
echo "### A: single 20MP hevc_qsv (H.264 can't — width>4096) ###"
t0=$(now)
$FF -f rawvideo -pix_fmt rgb24 -s ${W}x${H} -r $FR -i $RAW -c:v hevc_qsv -global_quality 25 -y A.hevc
t1=$(now); wa=$(el $t0 $t1)
echo "  hevc_qsv 20MP | $(fps $wa) fps | $(kbpf $(sz A.hevc)) KB/frame | wall ${wa}s"
rm -f A.hevc

# one tile alone (reference rate)
echo ""
echo "### one 2560x1920 h264_qsv tile (reference) ###"
t0=$(now)
$FF -f rawvideo -pix_fmt rgb24 -s ${W}x${H} -r $FR -i $RAW -vf crop=${TW}:${TH}:0:0 -c:v h264_qsv -global_quality 25 -y t0.h264
t1=$(now); w1=$(el $t0 $t1)
echo "  1 tile        | $(fps $w1) fps | $(kbpf $(sz t0.h264)) KB/frame/tile | wall ${w1}s"
rm -f t0.h264

echo ""
echo "### B-seq: 4 h264_qsv tiles SEQUENTIALLY ###"
t0=$(now); tot=0
for off in "0:0" "2560:0" "0:1920" "2560:1920"; do
  $FF -f rawvideo -pix_fmt rgb24 -s ${W}x${H} -r $FR -i $RAW -vf crop=${TW}:${TH}:${off} -c:v h264_qsv -global_quality 25 -y "seq_${off/:/_}.h264"
done
t1=$(now); ws=$(el $t0 $t1)
for f in seq_*.h264; do tot=$((tot+$(sz $f))); done
echo "  4 tiles seq   | $(fps $ws) fps | $(kbpf $tot) KB/frame (4 tiles sum) | wall ${ws}s"
rm -f seq_*.h264

echo ""
echo "### B-par: 4 h264_qsv tiles in PARALLEL ###"
t0=$(now)
for off in "0:0" "2560:0" "0:1920" "2560:1920"; do
  $FF -f rawvideo -pix_fmt rgb24 -s ${W}x${H} -r $FR -i $RAW -vf crop=${TW}:${TH}:${off} -c:v h264_qsv -global_quality 25 -y "par_${off/:/_}.h264" &
done
wait
t1=$(now); wp=$(el $t0 $t1); tot=0
for f in par_*.h264; do tot=$((tot+$(sz $f))); done
echo "  4 tiles par   | $(fps $wp) fps | $(kbpf $tot) KB/frame (4 tiles sum) | wall ${wp}s"
rm -f par_*.h264

rm -f $RAW
echo ""
echo "parallel speedup vs sequential: $(awk "BEGIN{printf \"%.2fx\", $ws/$wp}")"
