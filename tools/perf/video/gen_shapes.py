#!/usr/bin/env python
# gen_shapes.py W H N OUT [nshapes] [seed]
# Emit N raw rgb24 frames of random colored circles+squares that drift frame to
# frame (temporal coherence, like parts moving on an inspection line). Draws only
# each shape's bounding box so 20MP stays fast. Deterministic (seeded).
import sys, numpy as np

W, H, N, OUT = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
NS   = int(sys.argv[5]) if len(sys.argv) > 5 else 40
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 1
rng  = np.random.default_rng(seed)

# shape state
kind = rng.integers(0, 2, NS)                       # 0 circle, 1 square
cx   = rng.uniform(0, W, NS)
cy   = rng.uniform(0, H, NS)
rad  = rng.uniform(min(W, H) * 0.02, min(W, H) * 0.08, NS)
col  = rng.integers(0, 256, (NS, 3)).astype(np.uint8)
vx   = rng.uniform(-6, 6, NS)
vy   = rng.uniform(-6, 6, NS)

# static mild background gradient (gives the codec low-freq content, not flat)
yy = np.linspace(20, 60, H, dtype=np.uint8)[:, None]
xx = np.linspace(20, 60, W, dtype=np.uint8)[None, :]
bg = np.empty((H, W, 3), np.uint8)
bg[..., 0] = yy; bg[..., 1] = xx; bg[..., 2] = 40

with open(OUT, "wb") as f:
    for _ in range(N):
        frame = bg.copy()
        # move + bounce
        cx[:] += vx; cy[:] += vy
        vx[(cx < 0) | (cx >= W)] *= -1
        vy[(cy < 0) | (cy >= H)] *= -1
        cx[:] = np.clip(cx, 0, W - 1); cy[:] = np.clip(cy, 0, H - 1)
        for i in range(NS):
            r = int(rad[i]); x0 = int(cx[i]); y0 = int(cy[i])
            a = max(0, x0 - r); b = min(W, x0 + r + 1)
            c = max(0, y0 - r); d = min(H, y0 + r + 1)
            if b <= a or d <= c:
                continue
            if kind[i] == 1:                       # square: fill bbox
                frame[c:d, a:b] = col[i]
            else:                                  # circle: masked bbox
                ys = np.arange(c, d)[:, None] - y0
                xs = np.arange(a, b)[None, :] - x0
                m = (xs * xs + ys * ys) <= r * r
                frame[c:d, a:b][m] = col[i]
        f.write(frame.tobytes())
