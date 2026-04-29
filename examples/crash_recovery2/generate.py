"""Generate 10 noisy gradient grayscale frames with a known number of bright blobs.

5 happy frames (5 blobs each, under default 8 budget).
5 crash frames (12 blobs each, over default 8 budget).
Order is mixed.

Each blob is a filled bright disc on top of a noisy gradient bg. The
plugin thresholds at 127 and counts 8-connected components.
"""
import json
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).parent
FRAMES_DIR = ROOT / "frames"
GT_PATH = ROOT / "ground_truth.json"

W, H = 320, 240
BLOB_RADIUS = 8
SEED = 0xC0FFEE


def make_bg():
    rng = np.random.default_rng()
    # Gradient that stays well below 127 so it never thresholds in.
    xs = np.linspace(0, 80, W, dtype=np.float32)
    ys = np.linspace(0, 30, H, dtype=np.float32)
    bg = ys[:, None] + xs[None, :]
    bg += rng.normal(0, 6, size=bg.shape)  # noise
    bg = np.clip(bg, 0, 110).astype(np.uint8)
    return bg


def place_blobs(img: np.ndarray, n: int, rng: random.Random):
    pil = Image.fromarray(img, mode="L")
    draw = ImageDraw.Draw(pil)
    placed = []
    margin = BLOB_RADIUS + 2
    tries = 0
    while len(placed) < n and tries < 5000:
        tries += 1
        x = rng.randint(margin, W - margin - 1)
        y = rng.randint(margin, H - margin - 1)
        # keep blobs separated so the connected-components count matches n
        if any((px - x) ** 2 + (py - y) ** 2 < (2 * BLOB_RADIUS + 4) ** 2
               for px, py in placed):
            continue
        draw.ellipse(
            [x - BLOB_RADIUS, y - BLOB_RADIUS, x + BLOB_RADIUS, y + BLOB_RADIUS],
            fill=255,
        )
        placed.append((x, y))
    return np.array(pil), placed


def main():
    FRAMES_DIR.mkdir(exist_ok=True)
    # mixed schedule: H=happy(5 blobs), C=crash(12 blobs)
    plan = [
        ("frame_00.png", 5),    # H
        ("frame_01.png", 12),   # C
        ("frame_02.png", 5),    # H
        ("frame_03.png", 5),    # H
        ("frame_04.png", 12),   # C
        ("frame_05.png", 12),   # C
        ("frame_06.png", 5),    # H
        ("frame_07.png", 5),    # H
        ("frame_08.png", 12),   # C
        ("frame_09.png", 12),   # C
    ]

    rng = random.Random(SEED)
    gt = []
    for fname, n in plan:
        bg = make_bg()
        img, placed = place_blobs(bg, n, rng)
        assert len(placed) == n, f"could not place all {n} blobs in {fname}"
        Image.fromarray(img, mode="L").save(FRAMES_DIR / fname)
        gt.append({"file": fname, "blob_count": n})

    with open(GT_PATH, "w") as f:
        json.dump({"frames": gt, "crash_when_count_above_default": 8}, f, indent=2)

    happy = sum(1 for fr in gt if fr["blob_count"] <= 8)
    crash = sum(1 for fr in gt if fr["blob_count"] > 8)
    print(f"wrote {len(gt)} frames to {FRAMES_DIR}")
    print(f"  happy frames (count <= 8): {happy}")
    print(f"  crash frames (count >  8): {crash}")
    print(f"ground truth -> {GT_PATH}")


if __name__ == "__main__":
    main()
