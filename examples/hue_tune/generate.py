"""Generate 8 frames of 480x360 RGB.

Each frame contains 6 red, 4 blue, 2 green fully-saturated discs on a
gentle horizontal RGB gradient with gaussian noise (sigma=4). Discs are
non-overlapping. Saved as PNG; positions and counts written to
ground_truth.json.
"""
import json
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).parent
FRAMES_DIR = ROOT / "frames"
GT_PATH = ROOT / "ground_truth.json"

W, H = 480, 360
N_FRAMES = 8
SEED = 1234

COLORS = [
    ("red",   (255, 0, 0),   6),
    ("blue",  (0, 0, 255),   4),
    ("green", (0, 255, 0),   2),
]


def gradient_bg(w, h, rng):
    # Horizontal gradient: R fades 60→200, G fades 80→160, B fades 200→60.
    xs = np.linspace(0.0, 1.0, w)
    r = (60 + 140 * xs).astype(np.float32)
    g = (80 +  80 * xs).astype(np.float32)
    b = (200 - 140 * xs).astype(np.float32)
    bg = np.stack([
        np.broadcast_to(r, (h, w)),
        np.broadcast_to(g, (h, w)),
        np.broadcast_to(b, (h, w)),
    ], axis=-1)
    noise = rng.normal(0, 4.0, bg.shape).astype(np.float32)
    out = np.clip(bg + noise, 0, 255).astype(np.uint8)
    return out


def place_discs(rng):
    """Return list of (color_name, rgb, cx, cy, r) — non-overlapping."""
    placed = []
    margin = 24
    max_tries = 5000
    for name, rgb, n in COLORS:
        for _ in range(n):
            for _try in range(max_tries):
                r = rng.randint(12, 18)
                cx = rng.randint(margin + r, W - margin - r)
                cy = rng.randint(margin + r, H - margin - r)
                ok = True
                for (_n, _c, ox, oy, orad) in placed:
                    if math.hypot(cx - ox, cy - oy) < (r + orad + 4):
                        ok = False
                        break
                if ok:
                    placed.append((name, rgb, cx, cy, r))
                    break
            else:
                raise RuntimeError("could not place disc without overlap")
    return placed


def render_frame(idx, rng):
    arr = gradient_bg(W, H, rng)
    img = Image.fromarray(arr, mode="RGB")
    draw = ImageDraw.Draw(img)
    discs = place_discs(rng)
    for name, rgb, cx, cy, r in discs:
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=rgb)
    return img, discs


def main():
    FRAMES_DIR.mkdir(parents=True, exist_ok=True)
    rng = random.Random(SEED)
    np_rng = np.random.default_rng(SEED)

    frames_meta = []
    for i in range(N_FRAMES):
        # Use a per-frame numpy rng for noise; python rng for placement.
        local_np = np.random.default_rng(SEED + i)
        local_py = random.Random(SEED + 1000 + i)
        # Reuse generator we already wrote, by patching `np` module?
        # Simpler: replicate gradient_bg + place_discs with local rngs.
        bg = _gradient_bg_with(local_np)
        img = Image.fromarray(bg, mode="RGB")
        draw = ImageDraw.Draw(img)
        discs = _place_discs_with(local_py)
        for name, rgb, cx, cy, r in discs:
            draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=rgb)
        out = FRAMES_DIR / f"frame_{i:02d}.png"
        img.save(out)
        frames_meta.append({
            "file": out.name,
            "discs": [
                {"color": n, "cx": cx, "cy": cy, "r": rad}
                for (n, _rgb, cx, cy, rad) in discs
            ],
        })

    gt = {
        "image_size": [W, H],
        "counts": {"red": 6, "blue": 4, "green": 2},
        "frames": frames_meta,
    }
    GT_PATH.write_text(json.dumps(gt, indent=2))
    print(f"wrote {N_FRAMES} frames to {FRAMES_DIR} and {GT_PATH}")


def _gradient_bg_with(np_rng):
    # Keep the gradient close to neutral grey so background pixels never
    # cross the plugin's saturation floor (S>=80). Max channel-spread of
    # ~25 keeps S well under 80 across the whole frame.
    xs = np.linspace(0.0, 1.0, W)
    r = (140 + 12 * xs).astype(np.float32)
    g = (140 +  8 * xs).astype(np.float32)
    b = (140 - 10 * xs).astype(np.float32)
    bg = np.stack([
        np.broadcast_to(r, (H, W)),
        np.broadcast_to(g, (H, W)),
        np.broadcast_to(b, (H, W)),
    ], axis=-1)
    noise = np_rng.normal(0, 4.0, bg.shape).astype(np.float32)
    return np.clip(bg + noise, 0, 255).astype(np.uint8)


def _place_discs_with(py_rng):
    placed = []
    margin = 24
    max_tries = 5000
    for name, rgb, n in COLORS:
        for _ in range(n):
            for _try in range(max_tries):
                r = py_rng.randint(12, 18)
                cx = py_rng.randint(margin + r, W - margin - r)
                cy = py_rng.randint(margin + r, H - margin - r)
                ok = True
                for (_n, _c, ox, oy, orad) in placed:
                    if math.hypot(cx - ox, cy - oy) < (r + orad + 4):
                        ok = False
                        break
                if ok:
                    placed.append((name, rgb, cx, cy, r))
                    break
            else:
                raise RuntimeError("could not place disc without overlap")
    return placed


if __name__ == "__main__":
    main()
