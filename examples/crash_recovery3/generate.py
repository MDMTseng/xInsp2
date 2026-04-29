"""Generate 10 320x240 grayscale PNGs with noisy gradient bg + bright blobs.

Some frames have ~5 blobs (under crash budget=8 → happy), some have ~12
(over → plugin will crash). Ground truth records the blob count per frame.
"""
import json
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).parent
FRAMES = ROOT / "frames"
GT = ROOT / "ground_truth.json"

W, H = 320, 240
BLOB_R = 10
THRESHOLD = 128  # plugin uses fixed 128

# (filename, blob_count). Mix order: 6 happy (5 blobs), 4 crashing (12 blobs).
# Ensure crashes are interleaved AND consecutive at one point to prove the
# worker survives multiple crashes in a row.
PLAN = [
    ("frame_00.png",  5),   # happy
    ("frame_01.png", 12),   # crash
    ("frame_02.png",  5),   # happy
    ("frame_03.png",  5),   # happy
    ("frame_04.png", 12),   # crash
    ("frame_05.png", 12),   # crash (back-to-back with previous)
    ("frame_06.png",  5),   # happy
    ("frame_07.png", 12),   # crash
    ("frame_08.png",  5),   # happy
    ("frame_09.png",  5),   # happy
]


def gen_frame(seed: int, n_blobs: int) -> np.ndarray:
    rng = random.Random(seed)
    # Noisy gradient background, but kept WELL below threshold=128 so it
    # cannot accidentally produce blobs.
    nx = np.linspace(0, 1, W)
    ny = np.linspace(0, 1, H)
    grad = np.outer(ny * 0.5 + 0.2, np.ones(W)) * 60.0   # 12..42
    grad += np.outer(np.ones(H), nx * 30.0)              # +0..30
    np_rng = np.random.default_rng(seed)
    noise = np_rng.normal(0, 8.0, size=(H, W))
    bg = np.clip(grad + noise, 0, 110).astype(np.uint8)  # max 110, < 128

    img = Image.fromarray(bg, mode="L")
    draw = ImageDraw.Draw(img)
    placed = []
    attempts = 0
    while len(placed) < n_blobs and attempts < 1000:
        attempts += 1
        x = rng.randint(BLOB_R + 4, W - BLOB_R - 4)
        y = rng.randint(BLOB_R + 4, H - BLOB_R - 4)
        # keep blobs well separated so connectedComponents counts them all
        if any((x - px) ** 2 + (y - py) ** 2 < (BLOB_R * 3) ** 2
               for px, py in placed):
            continue
        # Bright filled circle, value 240 — well above threshold=128.
        draw.ellipse([x - BLOB_R, y - BLOB_R, x + BLOB_R, y + BLOB_R],
                     fill=240)
        placed.append((x, y))
    if len(placed) != n_blobs:
        raise RuntimeError(f"only placed {len(placed)} of {n_blobs} blobs")
    return np.array(img, dtype=np.uint8)


def main():
    FRAMES.mkdir(exist_ok=True)
    gt = []
    for seed, (fname, n) in enumerate(PLAN):
        arr = gen_frame(seed * 17 + 1, n)
        Image.fromarray(arr, mode="L").save(FRAMES / fname)
        gt.append({"file": fname, "blob_count": n})
        print(f"  {fname}: {n} blobs")
    GT.write_text(json.dumps({"frames": gt, "threshold_default": 8}, indent=2))
    print(f"wrote {len(PLAN)} frames -> {FRAMES}")
    print(f"ground truth -> {GT}")


if __name__ == "__main__":
    main()
