#!/usr/bin/env python
# gen_atlas.py W H N OUT COLS ROWS UPDATE_K [seed]
# Atlas of COLS*ROWS preview slots packed into one WxH frame. Frame 0 draws all
# slots; each later frame REDRAWS only UPDATE_K random slots (a fresh little
# scene), leaving the rest byte-identical to the previous frame. Models "a fixed
# grid of preview sources where only some update per tick" — the case temporal
# video coding should win big. Emits raw rgb24.
import sys, numpy as np
W, H, N, OUT = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
COLS, ROWS, K = int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7])
seed = int(sys.argv[8]) if len(sys.argv) > 8 else 1
rng = np.random.default_rng(seed)
cw, ch = W // COLS, H // ROWS
NS = COLS * ROWS

def draw_slot(buf, x0, y0, w, h):
    # a little inspection-ish scene: bg tint + a few random shapes
    buf[y0:y0+h, x0:x0+w] = rng.integers(20, 70, 3).astype(np.uint8)
    for _ in range(5):
        col = rng.integers(0, 256, 3).astype(np.uint8)
        r = int(rng.uniform(min(w, h)*0.08, min(w, h)*0.25))
        cx = int(rng.uniform(r, w - r)); cy = int(rng.uniform(r, h - r))
        a, b, c, d = x0+cx-r, x0+cx+r+1, y0+cy-r, y0+cy+r+1
        if rng.integers(0, 2):
            buf[c:d, a:b] = col
        else:
            ys = np.arange(c, d)[:, None]-(y0+cy); xs = np.arange(a, b)[None, :]-(x0+cx)
            m = xs*xs + ys*ys <= r*r
            buf[c:d, a:b][m] = col

frame = np.zeros((H, W, 3), np.uint8)
slots = [(i % COLS * cw, i // COLS * ch) for i in range(NS)]
for (x0, y0) in slots:              # frame 0: all slots
    draw_slot(frame, x0, y0, cw, ch)
with open(OUT, "wb") as f:
    f.write(frame.tobytes())
    for _ in range(N - 1):
        upd = rng.choice(NS, size=min(K, NS), replace=False)
        for si in upd:
            x0, y0 = slots[si]
            draw_slot(frame, x0, y0, cw, ch)
        f.write(frame.tobytes())
