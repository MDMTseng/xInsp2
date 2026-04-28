"""Generate the size-bucketed-circle case.

Each frame has 8-12 dark circles on a noisy gradient background. Each
circle's radius falls into one of three buckets: small (5-8), medium
(11-14), large (18-22). Ground truth: per-frame count by bucket.

Goal harder than circle_counting: count ALONE is not enough; agent
must size-classify each detected blob too.
"""
import json, os, random
import numpy as np
from PIL import Image

W, H = 640, 480
N_FRAMES = 15
NOISE_SIGMA = 10.0

random.seed(11)
np.random.seed(11)

BUCKETS = {
    "small":  (5,  8),
    "medium": (11, 14),
    "large":  (18, 22),
}

out = os.path.dirname(os.path.abspath(__file__))
frames_dir = os.path.join(out, "frames")
os.makedirs(frames_dir, exist_ok=True)

gt = {"width": W, "height": H, "buckets": BUCKETS, "frames": []}

for idx in range(N_FRAMES):
    bg = np.tile(np.linspace(60, 200, W, dtype=np.float32), (H, 1))
    img = bg.copy()
    counts = {b: 0 for b in BUCKETS}
    circles = []
    target = random.randint(8, 12)
    placed = 0
    attempts = 0
    while placed < target and attempts < 1000:
        attempts += 1
        bucket = random.choice(list(BUCKETS.keys()))
        r_min, r_max = BUCKETS[bucket]
        r = random.randint(r_min, r_max)
        cx = random.randint(r + 4, W - r - 4)
        cy = random.randint(r + 4, H - r - 4)
        # No overlap.
        if any((cx - c["cx"]) ** 2 + (cy - c["cy"]) ** 2 < (r + c["r"] + 6) ** 2 for c in circles):
            continue
        yy, xx = np.ogrid[:H, :W]
        mask = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
        local_bg = float(bg[cy, cx])
        img[mask] = max(0.0, local_bg - 60.0)
        circles.append({"cx": cx, "cy": cy, "r": r, "bucket": bucket})
        counts[bucket] += 1
        placed += 1
    img += np.random.normal(0, NOISE_SIGMA, img.shape).astype(np.float32)
    img = np.clip(img, 0, 255).astype(np.uint8)
    name = f"frame_{idx:02d}.png"
    Image.fromarray(img, mode="L").save(os.path.join(frames_dir, name))
    gt["frames"].append({"file": name, "counts": counts, "circles": circles})
    print(f"wrote {name}: {counts}")

with open(os.path.join(out, "ground_truth.json"), "w") as f:
    json.dump(gt, f, indent=2)
print("wrote ground_truth.json")
