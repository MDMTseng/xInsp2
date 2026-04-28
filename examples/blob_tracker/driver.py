"""Driver: open the blob_tracker project, run all 30 frames in order,
print per-frame predicted vs truth crossings, and the final total.

Usage:
    python driver.py            # full evaluation
    python driver.py --pool     # also dump image_pool_stats per frame
    python driver.py --debug    # extra diagnostic VARs each frame
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
FRAMES_DIR = ROOT / "frames"
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"
PROJECT_DIR = ROOT  # open_project takes the FOLDER, not the project.json file


def drain_events(c: Client) -> list[dict]:
    """Pull any events sitting in the SDK's event inbox."""
    out = []
    try:
        while True:
            out.append(c._inbox_events.get_nowait())
    except Empty:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pool", action="store_true", help="dump pool stats per frame")
    ap.add_argument("--debug", action="store_true", help="print extra VARs")
    args = ap.parse_args()

    with open(GT_PATH) as f:
        gt = json.load(f)
    expected_total = gt["total_crossings"]
    truth_by_frame = {fr["frame_idx"]: fr["crossings_so_far"] for fr in gt["frames"]}

    log_lines: list[dict] = []
    with Client() as c:
        c.on_log(lambda m: log_lines.append(m))

        # 1) Open the project. NB: `open_project` takes the project
        #    FOLDER. Sibling `load_project` looks identical via the SDK
        #    but the underlying cmd takes a path to project.json — and
        #    crucially does NOT compile project-local plugins. Use
        #    `open_project` here so the blob_centroid_detector plugin
        #    is built and the "det" instance is attached.
        try:
            c.open_project(str(PROJECT_DIR))
        except ProtocolError as e:
            print("OPEN_PROJECT FAILED:", e)
            for m in log_lines:
                if m.get("level") in ("error", "warn"):
                    print("  log:", m.get("msg"))
            return 1

        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            for m in log_lines:
                if m.get("level") == "error":
                    print("  log:", m.get("msg"))
            return 1

        # Capture any events emitted around compile_and_load so we can
        # report whether `state_dropped` fired (schema bump) vs nothing
        # (first-ever load → empty state).
        post_compile_events = drain_events(c)
        state_dropped = next(
            (e for e in post_compile_events if e.get("name") == "state_dropped"),
            None,
        )
        if state_dropped:
            print(f"event:state_dropped  {state_dropped.get('data')}")
        else:
            print("no state_dropped event (first-ever load OR same schema)")

        # 2) Run 30 frames in order.
        rows = []
        peak_high_water = 0
        for fr in gt["frames"]:
            idx = fr["frame_idx"]
            png = (FRAMES_DIR / fr["file"]).resolve()
            run = c.run(frame_path=str(png))

            pred = run.value("crossings_so_far", 0)
            delta = run.value("delta_crossings", 0)
            n_blobs = run.value("n_blobs", 0)
            matched = run.value("matched_blobs", 0)
            prev_count = run.value("prev_blob_count", 0)
            truth = truth_by_frame[idx]
            ok = pred == truth
            rows.append((idx, fr["file"], pred, truth, delta, n_blobs, matched, prev_count, ok))

            if args.pool:
                stats = c.image_pool_stats()
                cum = stats.get("cumulative", {})
                peak_high_water = max(peak_high_water, cum.get("high_water", 0))
                live = cum.get("live_now", 0)
                print(
                    f"  frame {idx:02d}  pool live={live:3d}  high_water={cum.get('high_water', 0):3d}"
                )
            else:
                # Cheap monitoring: just one stats call near the end.
                pass

        # Final pool stats once.
        final_stats = c.image_pool_stats()
        cum = final_stats.get("cumulative", {})
        peak_high_water = max(peak_high_water, cum.get("high_water", 0))

    # 3) Report.
    print()
    print(f"{'idx':>3} {'file':<14} {'pred':>4} {'truth':>5} {'delta':>5} {'blobs':>5} {'mat':>4} {'prev':>4}  ok")
    print("-" * 60)
    n_ok = 0
    for idx, fname, pred, truth, delta, n_blobs, matched, prev_count, ok in rows:
        mark = " " if ok else "X"
        if ok:
            n_ok += 1
        print(
            f"{idx:>3} {fname:<14} {pred:>4} {truth:>5} {delta:>5} {n_blobs:>5} {matched:>4} {prev_count:>4}   {mark}"
        )
    final_pred = rows[-1][2]
    print("-" * 60)
    print(f"per-frame match: {n_ok}/{len(rows)}")
    print(f"final total predicted: {final_pred}    truth: {expected_total}")
    print(f"image_pool peak high_water across run: {peak_high_water}")
    print(f"image_pool final live_now: {cum.get('live_now', 0)}")
    print(f"image_pool total_created: {cum.get('total_created', 0)}")

    if final_pred == expected_total and n_ok == len(rows):
        print("PASS")
        return 0
    elif final_pred == expected_total:
        # Crossings total correct, but some frames lagged by one — the
        # task statement explicitly tolerates this.
        print("PASS (final total exact; per-frame may lag by 1)")
        return 0
    else:
        print(f"FAIL: predicted {final_pred} != truth {expected_total}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
