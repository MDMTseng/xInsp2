"""Generate the blob-tracker sequence: 30 frames of dark blobs
moving left → right. Goal of the inspection: count how many
distinct blobs cross the vertical gate line at x=320 over the
sequence.

Each blob has a deterministic trajectory (start x, y, speed).
Across the 30 frames they advance; some never reach the gate,
some cross it once, none cross twice (frames don't span enough
time for a wrap-around). Ground truth: per-frame {crossings_so_far}
+ overall {total_crossings}.

Forces a script to:
  - Detect blobs each frame.
  - Match them to last-frame's blobs (xi::state cross-frame storage).
  - Detect prev_x < 320 AND cur_x >= 320 → a crossing event.
"""
import json, os, random
import numpy as np
from PIL import Image

W, H = 640, 480
N_FRAMES = 30
GATE_X = 320
NOISE_SIGMA = 8.0
RADIUS = 14

random.seed(23)
np.random.seed(23)

# Five blobs, each a dict with start_x, y, speed (px/frame).
# Tuned so 3 of them cross the gate in [0, N_FRAMES), one never reaches
# it, one starts past it (no crossing).
blobs = [
    {"start_x":  40, "y": 100, "speed": 12},   # crosses around frame ~24
    {"start_x":  20, "y": 200, "speed": 18},   # crosses around frame ~17
    {"start_x":   0, "y": 320, "speed": 20},   # crosses around frame ~16
    {"start_x":   0, "y":  60, "speed":  4},   # never reaches gate
    {"start_x": 360, "y": 400, "speed": 10},   # already past gate at start
]

out = os.path.dirname(os.path.abspath(__file__))
frames_dir = os.path.join(out, "frames")
os.makedirs(frames_dir, exist_ok=True)

gt = {"width": W, "height": H, "gate_x": GATE_X, "frames": []}
crossings = 0
prev_xs = [b["start_x"] for b in blobs]

for f in range(N_FRAMES):
    bg = np.tile(np.linspace(60, 200, W, dtype=np.float32), (H, 1))
    img = bg.copy()

    cur_xs = []
    for i, b in enumerate(blobs):
        x = b["start_x"] + b["speed"] * f
        cur_xs.append(x)
        if 0 <= x < W:
            yy, xx = np.ogrid[:H, :W]
            mask = (xx - x) ** 2 + (yy - b["y"]) ** 2 <= RADIUS * RADIUS
            local_bg = float(bg[b["y"], min(int(x), W - 1)])
            img[mask] = max(0.0, local_bg - 60.0)
        # Crossing event: previous frame on left, this frame on right.
        if prev_xs[i] < GATE_X and x >= GATE_X:
            crossings += 1

    img += np.random.normal(0, NOISE_SIGMA, img.shape).astype(np.float32)
    img = np.clip(img, 0, 255).astype(np.uint8)
    name = f"frame_{f:02d}.png"
    Image.fromarray(img, mode="L").save(os.path.join(frames_dir, name))

    gt["frames"].append({
        "file": name, "frame_idx": f,
        "blob_xs": [round(x, 1) for x in cur_xs],
        "crossings_so_far": crossings,
    })
    prev_xs = cur_xs

gt["total_crossings"] = crossings
with open(os.path.join(out, "ground_truth.json"), "w") as fp:
    json.dump(gt, fp, indent=2)
print(f"wrote {N_FRAMES} frames; total_crossings={crossings}")
