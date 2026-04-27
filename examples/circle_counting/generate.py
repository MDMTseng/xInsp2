"""Generate synthetic test frames for the circle-counting case.

Each frame: 640x480 grayscale with
  - linear horizontal gradient background (dark left -> bright right)
  - N filled circles, each ~60 darker than local bg (so global threshold fails)
  - additive gaussian noise

Writes frame_NN.png + ground_truth.json (count + circle centers/radii).
"""
import json, os, random
import numpy as np
from PIL import Image

random.seed(42)
np.random.seed(42)

W, H = 640, 480
COUNTS = [10] * 20
out = os.path.dirname(os.path.abspath(__file__))
frames_dir = os.path.join(out, "frames")
os.makedirs(frames_dir, exist_ok=True)

gt = {"width": W, "height": H, "frames": []}

for idx, n in enumerate(COUNTS):
    bg = np.tile(np.linspace(50, 200, W, dtype=np.float32), (H, 1))
    img = bg.copy()

    circles = []
    placed = 0
    attempts = 0
    while placed < n and attempts < 500:
        attempts += 1
        r = random.randint(14, 26)
        cx = random.randint(r + 2, W - r - 2)
        cy = random.randint(r + 2, H - r - 2)
        if any((cx - c["cx"]) ** 2 + (cy - c["cy"]) ** 2 < (r + c["r"] + 4) ** 2 for c in circles):
            continue
        yy, xx = np.ogrid[:H, :W]
        mask = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
        local_bg = float(bg[cy, cx])
        circle_val = max(0.0, local_bg - 60.0)
        img[mask] = circle_val
        circles.append({"cx": cx, "cy": cy, "r": r})
        placed += 1

    noise = np.random.normal(0, 12, img.shape).astype(np.float32)
    img = np.clip(img + noise, 0, 255).astype(np.uint8)

    name = f"frame_{idx:02d}.png"
    Image.fromarray(img, mode="L").save(os.path.join(frames_dir, name))
    gt["frames"].append({"file": name, "count": n, "circles": circles})
    print(f"wrote {name} count={n}")

with open(os.path.join(out, "ground_truth.json"), "w") as f:
    json.dump(gt, f, indent=2)
print("wrote ground_truth.json")
