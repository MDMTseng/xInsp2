"""Prototype the algorithm in pure Python/numpy to set parameters."""
import json
from pathlib import Path
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

ROOT = Path(__file__).parent
FRAMES_DIR = ROOT / "frames"
GT_PATH = ROOT / "ground_truth.json"

def boxblur(img, r):
    # cumulative sum approach
    return ndi.uniform_filter(img.astype(np.float32), size=2*r+1)

def count_circles(gray, blur_r=2, block_r=60, C=15, close_r=4, min_a=400, max_a=3500):
    g = boxblur(gray, blur_r)
    mean = boxblur(g, block_r)
    # mark pixels darker than local mean by more than C
    binary = ((mean - g) > C).astype(np.uint8)
    # close (dilate then erode)
    if close_r > 0:
        st = ndi.generate_binary_structure(2,1)
        binary = ndi.binary_dilation(binary, iterations=close_r).astype(np.uint8)
        binary = ndi.binary_erosion(binary, iterations=close_r).astype(np.uint8)
    labels, n = ndi.label(binary)
    sizes = ndi.sum(binary, labels, range(1, n+1))
    keep = [s for s in sizes if min_a <= s <= max_a]
    return len(keep), n, sizes

with open(GT_PATH) as f:
    gt = json.load(f)

# Sweep
configs = []
for block_r in [40, 60, 80, 100]:
    for C in [5, 10, 15, 20, 25]:
        for close_r in [2, 3, 4, 5, 6]:
            errs = []
            for fr in gt["frames"]:
                arr = np.array(Image.open(FRAMES_DIR / fr["file"]).convert("L"))
                cnt, _, _ = count_circles(arr, block_r=block_r, C=C, close_r=close_r,
                                          min_a=300, max_a=4000)
                errs.append(abs(cnt - fr["count"]))
            avg = np.mean(errs)
            exact = sum(1 for e in errs if e == 0)
            configs.append((avg, -exact, block_r, C, close_r))

configs.sort()
print("top 10:")
for c in configs[:10]:
    print(c)
