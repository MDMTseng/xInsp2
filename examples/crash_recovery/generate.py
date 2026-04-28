"""Generate 10 grayscale 320x240 PNG frames + ground_truth.json.

Mix:
  - 7 "happy" frames with ~4-5 dark blobs (count <= 8)
  - 3 "crash" frames with ~12 dark blobs (count >  8 -> plugin segfaults)

Order interleaves so the driver hits crashes both at the start, middle,
and end of the loop. Seeded RNG so this is reproducible.
"""
from __future__ import annotations

import json
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).parent.resolve()
FRAMES = ROOT / "frames"
W, H = 320, 240


def make_bg(rng: random.Random) -> np.ndarray:
    # Bright noisy gradient: pixels live in ~[180, 230], well above the
    # threshold of 127 the plugin uses, so background never trips the mask.
    grad_x = np.linspace(180, 220, W, dtype=np.float32)
    bg = np.tile(grad_x, (H, 1))
    np_rng = np.random.default_rng(rng.randrange(1 << 30))
    bg += np_rng.normal(0.0, 6.0, size=bg.shape).astype(np.float32)
    bg = np.clip(bg, 0, 255).astype(np.uint8)
    return bg


def draw_blobs(bg: np.ndarray, n: int, rng: random.Random) -> np.ndarray:
    img = Image.fromarray(bg, mode="L")
    d = ImageDraw.Draw(img)
    placed: list[tuple[int, int, int]] = []
    margin = 12
    attempts = 0
    while len(placed) < n and attempts < 4000:
        attempts += 1
        r = rng.randint(8, 14)
        x = rng.randint(margin + r, W - margin - r)
        y = rng.randint(margin + r, H - margin - r)
        # No-overlap so each blob produces exactly one connected component.
        ok = True
        for px, py, pr in placed:
            if (px - x) ** 2 + (py - y) ** 2 < (pr + r + 4) ** 2:
                ok = False
                break
        if not ok:
            continue
        # Dark fill — well below threshold even after additive noise.
        d.ellipse([x - r, y - r, x + r, y + r], fill=20)
        placed.append((x, y, r))
    if len(placed) < n:
        raise RuntimeError(f"could only place {len(placed)} of {n} blobs")
    return np.array(img)


def main():
    FRAMES.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1234)

    # H H C H C H H C H H  -> 7 happy, 3 crash, mixed
    spec = [
        ("happy", 5),
        ("happy", 4),
        ("crash", 12),
        ("happy", 5),
        ("crash", 13),
        ("happy", 4),
        ("happy", 5),
        ("crash", 12),
        ("happy", 5),
        ("happy", 4),
    ]

    frames = []
    for i, (kind, n) in enumerate(spec):
        bg = make_bg(rng)
        arr = draw_blobs(bg, n, rng)
        fname = f"frame_{i:02d}.png"
        Image.fromarray(arr, mode="L").save(FRAMES / fname)
        frames.append({"file": fname, "blob_count": n, "kind": kind})

    gt = {
        "threshold": 127,
        "crash_when_count_above_default": 8,
        "frames": frames,
    }
    (ROOT / "ground_truth.json").write_text(json.dumps(gt, indent=2), encoding="utf-8")
    print(f"wrote {len(frames)} frames + ground_truth.json")
    print("happy:", [f["file"] for f in frames if f["kind"] == "happy"])
    print("crash:", [f["file"] for f in frames if f["kind"] == "crash"])


if __name__ == "__main__":
    main()
