"""live_tune — 10 frames each containing exactly N=8 dark circles
on a noisy gradient. Acceptance is strict: every frame must report
count == 8.

The wrinkle: the project ships a deliberately-broken initial plugin
(`circle_counter`) that hardcodes a too-aggressive erosion radius.
Out of the box it under-counts on most frames. The agent has to
edit the plugin source and call cmd:recompile_project_plugin (the
"linchpin for live tuning") in a loop until the count is right
across all frames.
"""
import json, os, random
import numpy as np
from PIL import Image

W, H = 640, 480
N_FRAMES = 10
TARGET_COUNT = 8
NOISE_SIGMA = 9.0

random.seed(41)
np.random.seed(41)

out = os.path.dirname(os.path.abspath(__file__))
frames_dir = os.path.join(out, "frames")
os.makedirs(frames_dir, exist_ok=True)

gt = {"target_count": TARGET_COUNT, "frames": []}
for f in range(N_FRAMES):
    bg = np.tile(np.linspace(60, 200, W, dtype=np.float32), (H, 1))
    img = bg.copy()
    placed_circles = []
    attempts = 0
    # Reject overlapping circles. The first agent on this case had to
    # compensate for figure-8 regions caused by the prior overlap-
    # permissive generator; cleaner to keep the dataset truthful.
    while len(placed_circles) < TARGET_COUNT and attempts < 1000:
        attempts += 1
        r = random.randint(11, 17)
        cx = random.randint(r + 4, W - r - 4)
        cy = random.randint(r + 4, H - r - 4)
        # Reject if too close to any already-placed circle. Min separation
        # is generous (radii sum + 6 px) so a small erosion can't merge.
        if any((cx - c["cx"]) ** 2 + (cy - c["cy"]) ** 2
               < (r + c["r"] + 6) ** 2 for c in placed_circles):
            continue
        yy, xx = np.ogrid[:H, :W]
        mask = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
        local_bg = float(bg[cy, cx])
        img[mask] = max(0.0, local_bg - 60.0)
        placed_circles.append({"cx": cx, "cy": cy, "r": r})
    img += np.random.normal(0, NOISE_SIGMA, img.shape).astype(np.float32)
    img = np.clip(img, 0, 255).astype(np.uint8)
    name = f"frame_{f:02d}.png"
    Image.fromarray(img, mode="L").save(os.path.join(frames_dir, name))
    gt["frames"].append({"file": name, "count": len(placed_circles)})

with open(os.path.join(out, "ground_truth.json"), "w") as fp:
    json.dump(gt, fp, indent=2)
print(f"wrote {N_FRAMES} frames; target_count={TARGET_COUNT}")
