"""Generate an object-locating-and-counting puzzle with escalating harsh
degradation. Deterministic (fixed seed) so ground truth is reproducible.

Each frame: N dark blobs (filled circles) on a light background at known
centroids, then degraded — Gaussian blur, additive Gaussian noise, salt &
pepper, contrast compression, and an uneven-illumination gradient — getting
harsher from frame 0 to the last.

Frames -> examples/object_count_puzzle/frames/*.png
Ground truth -> examples/object_count_puzzle/ground_truth.json (read by score_puzzle.py)

Deterministic (fixed seed): frames + ground truth regenerate identically, so
nothing binary needs to live in git (frames/ + ground_truth.json are gitignored).
"""
from __future__ import annotations
import json, os, math
from pathlib import Path
import numpy as np
import cv2

ROOT = Path(__file__).resolve().parent
FRAMES = ROOT / "frames"
FRAMES.mkdir(parents=True, exist_ok=True)
GT_DIR = ROOT

W = H = 512
rng = np.random.default_rng(20260530)   # fixed seed -> reproducible GT

def place_blobs(n):
    """Place n non-overlapping dark blobs; return (clean_img, centroids)."""
    img = np.full((H, W), 235, np.uint8)          # light background
    centroids = []
    tries = 0
    while len(centroids) < n and tries < 2000:
        tries += 1
        r = int(rng.integers(10, 22))
        x = int(rng.integers(r + 6, W - r - 6))
        y = int(rng.integers(r + 6, H - r - 6))
        if any((x - cx) ** 2 + (y - cy) ** 2 < (r + cr + 14) ** 2
               for cx, cy, cr in centroids):
            continue
        val = int(rng.integers(25, 70))            # dark fill
        cv2.circle(img, (x, y), r, val, -1)
        centroids.append((x, y, r))
    return img, [(cx, cy) for cx, cy, _ in centroids]

def degrade(img, level):
    """level in [0,1]; 0 = mild, 1 = harsh."""
    f = img.astype(np.float32)
    # uneven illumination: smooth additive gradient + vignette
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    grad = (xx / W - 0.5) * (60 * level) + (yy / H - 0.5) * (40 * level)
    vig = -((xx - W / 2) ** 2 + (yy - H / 2) ** 2) / (W * H) * (120 * level)
    f = f + grad + vig
    # contrast compression toward mid-grey
    c = 1.0 - 0.55 * level
    f = (f - 128) * c + 128
    # gaussian blur
    sig = 0.8 + 4.0 * level
    f = cv2.GaussianBlur(f, (0, 0), sig)
    # additive gaussian noise
    f = f + rng.normal(0, 10 + 58 * level, f.shape)
    out = np.clip(f, 0, 255).astype(np.uint8)
    # salt & pepper
    sp = rng.random((H, W))
    amt = 0.035 * level
    out[sp < amt / 2] = 0
    out[sp > 1 - amt / 2] = 255
    return out

def main():
    n_frames = 8
    gt = {"tolerance_px": 18, "frames": []}
    for i in range(n_frames):
        n = int(rng.integers(3, 13))
        clean, cents = place_blobs(n)
        level = i / (n_frames - 1)
        deg = degrade(clean, level)
        name = f"frame_{i:02d}.png"
        cv2.imwrite(str(FRAMES / name), deg)
        gt["frames"].append({"file": name, "level": round(level, 3),
                             "count": len(cents),
                             "centroids": [[int(x), int(y)] for x, y in cents]})
        print(f"  {name}: level={level:.2f} count={len(cents)}")
    (GT_DIR / "ground_truth.json").write_text(json.dumps(gt, indent=2))
    print(f"\nframes -> {FRAMES}")
    print(f"ground truth -> {GT_DIR / 'ground_truth.json'}")
    print(f"total objects across {n_frames} frames: "
          f"{sum(f['count'] for f in gt['frames'])}")

if __name__ == "__main__":
    main()
