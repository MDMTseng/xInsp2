"""trend_monitor — count circles per frame, flag frames whose count
deviates > 30% from the running mean of the previous 10 frames.

Generates 30 frames. Most frames have "normal" count drawn from a
narrow distribution; a few are seeded as ANOMALIES (count clearly
above or below the trend window). Goal: agent's pipeline reports
`flagged: true` exactly on those anomalies.

Forces the agent to use `xi::state()` to carry a rolling window of
recent counts across runs.
"""
import json, os, random
import numpy as np
from PIL import Image

W, H = 640, 480
N_FRAMES = 30
NOISE_SIGMA = 8.0

random.seed(31)
np.random.seed(31)

# Per-frame count plan: ~5 normal-ish counts; explicit anomalies at
# specific frames. Designed so that the rolling window has settled
# (frames 10+) before the first anomaly hits.
counts = [random.randint(4, 6) for _ in range(N_FRAMES)]
ANOMALY_FRAMES = {12: 12, 18: 1, 24: 11}   # frame_idx -> count
for f, n in ANOMALY_FRAMES.items():
    counts[f] = n

out = os.path.dirname(os.path.abspath(__file__))
frames_dir = os.path.join(out, "frames")
os.makedirs(frames_dir, exist_ok=True)

gt = {"width": W, "height": H, "anomaly_frames": sorted(ANOMALY_FRAMES.keys()),
      "frames": []}

# Place circles randomly per frame. Circles are dark (~60 below local bg).
for f, count in enumerate(counts):
    bg = np.tile(np.linspace(60, 200, W, dtype=np.float32), (H, 1))
    img = bg.copy()
    placed = 0
    attempts = 0
    while placed < count and attempts < 500:
        attempts += 1
        r = random.randint(10, 18)
        cx = random.randint(r + 4, W - r - 4)
        cy = random.randint(r + 4, H - r - 4)
        yy, xx = np.ogrid[:H, :W]
        mask = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
        local_bg = float(bg[cy, cx])
        img[mask] = max(0.0, local_bg - 60.0)
        placed += 1
    img += np.random.normal(0, NOISE_SIGMA, img.shape).astype(np.float32)
    img = np.clip(img, 0, 255).astype(np.uint8)
    name = f"frame_{f:02d}.png"
    Image.fromarray(img, mode="L").save(os.path.join(frames_dir, name))
    gt["frames"].append({"file": name, "frame_idx": f, "count": placed,
                         "is_anomaly": f in ANOMALY_FRAMES})

with open(os.path.join(out, "ground_truth.json"), "w") as fp:
    json.dump(gt, fp, indent=2)
print(f"wrote {N_FRAMES} frames; anomalies at {sorted(ANOMALY_FRAMES.keys())}")
