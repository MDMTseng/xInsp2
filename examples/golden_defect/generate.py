"""Generate the golden-reference defect-detection case.

Scenario: a fixed "good part" pattern (gradient + per-pixel deterministic
texture) is the reference. 20 test frames are sampled from the same
process — half are clean (texture + new noise), half have a defect
injected (small dark blob OR thin scratch). Goal of the inspection:
flag defective frames; localize defects when present.

Outputs:
  reference.png             — golden reference (no noise)
  frames/frame_NN.png       — 20 test frames
  ground_truth.json         — per-frame {defect: bool, type: blob|scratch|none, bbox}

Why this is harder than circle counting:
  - Per-frame texture noise means simple subtraction has many false
    positives. The inspection must be robust to noise σ but sensitive
    to a small (radius ~6) blob.
  - Subagent must build a plugin that *stores* the reference (set-up
    time) and a separate run-time comparator. UI must let the user
    load / clear the reference.
"""
import json, os, random
import numpy as np
from PIL import Image

W, H = 320, 240
N_FRAMES = 20
NOISE_SIGMA = 8.0

random.seed(7)
np.random.seed(7)

out = os.path.dirname(os.path.abspath(__file__))
frames_dir = os.path.join(out, "frames")
os.makedirs(frames_dir, exist_ok=True)

# Base = horizontal gradient + a fixed low-frequency texture (deterministic
# so the reference is meaningful — the comparator must subtract THIS,
# not just a flat plane).
gx = np.linspace(60, 190, W, dtype=np.float32)
gy = np.linspace(0, 20, H, dtype=np.float32)[:, None]
xx, yy = np.meshgrid(np.arange(W), np.arange(H))
texture = 8.0 * np.sin(xx / 23.0) * np.cos(yy / 17.0)
base = (gx[None, :] + gy + texture).astype(np.float32)

# Reference: base WITHOUT noise (the "ideal good part").
ref = np.clip(base, 0, 255).astype(np.uint8)
Image.fromarray(ref, mode="L").save(os.path.join(out, "reference.png"))

# Decide ahead of time which frames have defects (10 clean, 10 defect).
indices = list(range(N_FRAMES))
random.shuffle(indices)
defect_idx = set(indices[:N_FRAMES // 2])

gt = {"width": W, "height": H, "noise_sigma": NOISE_SIGMA, "frames": []}

for i in range(N_FRAMES):
    img = base.copy()
    # New noise per frame.
    img += np.random.normal(0, NOISE_SIGMA, img.shape).astype(np.float32)

    record = {"file": f"frame_{i:02d}.png", "defect": False, "type": "none", "bbox": None}

    if i in defect_idx:
        kind = random.choice(["blob", "scratch"])
        if kind == "blob":
            r = random.randint(5, 9)
            cx = random.randint(r + 5, W - r - 5)
            cy = random.randint(r + 5, H - r - 5)
            yy_, xx_ = np.ogrid[:H, :W]
            mask = (xx_ - cx) ** 2 + (yy_ - cy) ** 2 <= r * r
            # Blob is darker by ~50 (similar magnitude to circle case but smaller).
            img[mask] -= 50
            record.update(defect=True, type="blob",
                          bbox=[cx - r, cy - r, cx + r, cy + r])
        else:  # scratch
            length = random.randint(20, 40)
            x0 = random.randint(10, W - length - 10)
            y0 = random.randint(10, H - 10)
            angle = random.uniform(-0.4, 0.4)
            for t in range(length):
                x = int(x0 + t)
                y = int(y0 + t * angle)
                if 0 <= x < W and 0 <= y < H:
                    img[max(0, y-1):y+2, max(0, x-1):x+2] -= 35
            record.update(defect=True, type="scratch",
                          bbox=[x0, y0 - 2, x0 + length, y0 + 2])

    img = np.clip(img, 0, 255).astype(np.uint8)
    Image.fromarray(img, mode="L").save(os.path.join(frames_dir, record["file"]))
    gt["frames"].append(record)
    print(f"wrote {record['file']:14s} defect={record['defect']} type={record['type']}")

with open(os.path.join(out, "ground_truth.json"), "w") as f:
    json.dump(gt, f, indent=2)

print(f"\n{sum(1 for f in gt['frames'] if f['defect'])} defect / "
      f"{sum(1 for f in gt['frames'] if not f['defect'])} clean")
print("wrote reference.png + ground_truth.json")
