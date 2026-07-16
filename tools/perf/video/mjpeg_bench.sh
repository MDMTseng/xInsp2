#!/usr/bin/env bash
# mjpeg_bench.sh — hardware MJPEG (mjpeg_qsv) bench, GPU color-convert.
# Metrics: CPU cores (ffmpeg -benchmark), fps, KB/frame; plus a CONCURRENCY sweep
# (the point for a CPU-bound priority: does total CPU stay bounded as preview
# count grows, or scale linearly like CPU turbo-JPEG?).
set -u
N=60; FR=30
GPU="-init_hw_device qsv=hw -filter_hw_device hw"
VF="hwupload=extra_hw_frames=64,vpp_qsv=format=nv12"
sumcores(){ # args: list of benchmark-log files ; prints total (utime+stime); needs wall separately
  awk '/utime=/{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]~/utime/)u+=a[2]+0;if(a[1]~/stime/)s+=a[2]+0}} END{printf "%.2f", u+s}' "$@"
}

gen(){ python gen_shapes.py "$2" "$3" $N "$1" >/dev/null; cat "$1">/dev/null; }

single(){ # name W H
  local raw=r.raw; gen $raw $2 $3
  local log; log=$(ffmpeg -hide_banner -benchmark $GPU -f rawvideo -pix_fmt rgb24 -s $2x$3 -r $FR -i $raw -vf "$VF" -c:v mjpeg_qsv -global_quality 25 -y o.mjpg 2>&1 >/dev/null)
  local u s r; u=$(echo "$log"|grep -oE "utime=[0-9.]+"|tail -1|cut -d= -f2); s=$(echo "$log"|grep -oE "stime=[0-9.]+"|tail -1|cut -d= -f2); r=$(echo "$log"|grep -oE "rtime=[0-9.]+"|tail -1|cut -d= -f2)
  local kb; kb=$(awk "BEGIN{printf \"%.1f\", $(stat -c%s o.mjpg)/$N/1024}")
  awk "BEGIN{printf \"  %-6s | %.2f cores | %3.0f fps | %s KB/frame | per-frame %.1f ms\n\", \"$1\", ($u+$s)/$r, $N/$r, \"$kb\", $r/$N*1000}"
  rm -f o.mjpg r.raw
}

echo "=== hardware MJPEG (mjpeg_qsv), GPU convert — single encode ==="
echo "(read-only I/O floor ~1.0 core is included; real pipeline has the frame in RAM)"
single 5MP 2448 2048
single 20MP 5120 3840

echo ""
echo "=== CONCURRENCY sweep @5MP: K parallel mjpeg_qsv on the ONE iGPU ==="
gen c.raw 2448 2048
for K in 1 2 4 8; do
  rm -f bk_*.log
  t0=$(date +%s.%N)
  for i in $(seq 1 $K); do
    ffmpeg -hide_banner -benchmark $GPU -f rawvideo -pix_fmt rgb24 -s 2448x2048 -r $FR -i c.raw -vf "$VF" -c:v mjpeg_qsv -global_quality 25 -y "o_$i.mjpg" 2>"bk_$i.log" >/dev/null &
  done
  wait
  t1=$(date +%s.%N)
  wall=$(awk "BEGIN{print $t1-$t0}")
  tot=$(sumcores bk_*.log)
  cores=$(awk "BEGIN{printf \"%.2f\", $tot/$wall}")
  aggfps=$(awk "BEGIN{printf \"%.0f\", $K*$N/$wall}")
  perstream=$(awk "BEGIN{printf \"%.2f\", $tot/$wall/$K}")
  printf "  %d stream(s) | total %5s cores (%s/stream) | agg %4s fps | wall %.2fs\n" "$K" "$cores" "$perstream" "$aggfps" "$wall"
  rm -f o_*.mjpg bk_*.log
done
rm -f c.raw
echo ""
echo "contrast: CPU turbo-JPEG would need ~1 core PER stream (linear) — hence ~K cores."