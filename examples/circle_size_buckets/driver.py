"""Driver: open circle_size_buckets project, run all 15 frames, score per-bucket.

Halfway through it pokes c.image_pool_stats() and c.recent_errors() to
exercise the new observability tools.
"""
import json
import sys
import time
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent.resolve()
PROJECT = ROOT
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"
FRAMES_DIR = ROOT / "frames"
BUCKETS = ("small", "medium", "large")


def main():
    with open(GT_PATH) as f:
        gt = json.load(f)

    log_lines = []
    with Client() as c:
        c.on_log(lambda m: log_lines.append(m))

        # --- discover plugins; verify our manifest round-trips through cmd:list_plugins
        before_plugins = c.list_plugins()
        manifest_seen = {p["name"]: p.get("manifest") for p in before_plugins}

        print(f"opening project {PROJECT}")
        try:
            proj = c.call("open_project", {"folder": str(PROJECT)}, timeout=240)
        except ProtocolError as e:
            print("OPEN PROJECT FAILED:", e)
            for m in log_lines[-20:]:
                print("  log:", m.get("level"), m.get("msg"))
            sys.exit(2)

        plugins = [p.get("name") for p in proj.get("plugins", [])]
        instances = [(i.get("name"), i.get("plugin")) for i in proj.get("instances", [])]
        print(f"  plugins:   {plugins}")
        print(f"  instances: {instances}")

        # Re-list now that project plugins are loaded; manifest should be visible.
        after_plugins = c.list_plugins()
        proj_manifests = {p["name"]: p.get("manifest")
                          for p in after_plugins
                          if p.get("name") in ("local_contrast_detector", "size_bucket_counter")}
        print("  manifests visible after open_project:")
        for name, man in proj_manifests.items():
            if man:
                params = [p["name"] for p in man.get("params", [])]
                print(f"    {name}: params={params}")
            else:
                print(f"    {name}: (no manifest in cmd:list_plugins reply)")

        # Compile inspect.cpp.
        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            for m in log_lines[-30:]:
                if m.get("level") == "error":
                    print("  log:", m.get("msg"))
            sys.exit(3)

        # Score loop.
        results = []
        midpoint_pool = None
        midpoint_errors = None
        t0 = time.time()
        for i, fr in enumerate(gt["frames"]):
            fname = fr["file"]
            truth = fr["counts"]
            frame_path = str((FRAMES_DIR / fname).resolve())
            run = c.run(frame_path=frame_path, timeout=60)
            pred = {b: int(run.value(f"count_{b}", -1)) for b in BUCKETS}
            err = {b: abs(pred[b] - truth[b]) for b in BUCKETS}
            results.append({
                "file": fname, "truth": truth, "pred": pred, "err": err,
                "total_regions":  run.value("total_regions"),
                "rejected_small": run.value("rejected_small"),
                "rejected_big":   run.value("rejected_big"),
            })

            # Halfway: shake down the new observability commands.
            if i == 7:
                try:
                    midpoint_pool = c.image_pool_stats()
                except Exception as e:
                    midpoint_pool = {"error": str(e)}
                try:
                    midpoint_errors = c.recent_errors()
                except Exception as e:
                    midpoint_errors = {"error": str(e)}

        # End-of-run pool stats too.
        try:
            final_pool = c.image_pool_stats()
        except Exception as e:
            final_pool = {"error": str(e)}
        try:
            final_errors = c.recent_errors()
        except Exception as e:
            final_errors = {"error": str(e)}
        elapsed = time.time() - t0

    # ----------- score & write report -----------
    perfect_frames = 0
    bucket_total_err = {b: 0 for b in BUCKETS}
    for r in results:
        if all(r["err"][b] == 0 for b in BUCKETS):
            perfect_frames += 1
        for b in BUCKETS:
            bucket_total_err[b] += r["err"][b]

    print("\nResults")
    print(f"{'frame':<14}" + " ".join(f"{b}_t/p/e" for b in BUCKETS))
    for r in results:
        cells = []
        for b in BUCKETS:
            cells.append(f"{r['truth'][b]}/{r['pred'][b]}/{r['err'][b]}")
        print(f"{r['file']:<14}" + " ".join(f"{c:>10}" for c in cells))
    print(f"\nperfect frames: {perfect_frames}/{len(results)}")
    print(f"per-bucket total error: {bucket_total_err}")
    print(f"elapsed: {elapsed:.1f}s")

    # Write RESULTS.md
    write_results_md(
        results=results,
        perfect_frames=perfect_frames,
        bucket_total_err=bucket_total_err,
        elapsed=elapsed,
        manifest_seen_in_list_plugins=proj_manifests,
        midpoint_pool=midpoint_pool,
        midpoint_errors=midpoint_errors,
        final_pool=final_pool,
        final_errors=final_errors,
    )

    # Exit 0 only if criteria met.
    ok = (perfect_frames >= 12) and all(bucket_total_err[b] <= 5 for b in BUCKETS)
    sys.exit(0 if ok else 1)


def write_results_md(*, results, perfect_frames, bucket_total_err, elapsed,
                     manifest_seen_in_list_plugins,
                     midpoint_pool, midpoint_errors,
                     final_pool, final_errors):
    out = ROOT / "RESULTS.md"
    lines = []
    n = len(results)
    ok = (perfect_frames >= n - 3) and all(bucket_total_err[b] <= 5 for b in BUCKETS)
    lines.append("# RESULTS — circle_size_buckets")
    lines.append("")
    lines.append(f"- frames scored: **{n}**")
    lines.append(f"- perfect frames (all 3 buckets exact): **{perfect_frames}/{n}**")
    lines.append(f"- per-bucket total |pred-truth|: small={bucket_total_err['small']}, "
                 f"medium={bucket_total_err['medium']}, large={bucket_total_err['large']}")
    lines.append(f"- elapsed: {elapsed:.1f}s")
    lines.append(f"- targets: ≥12/15 perfect AND each bucket ≤5 → **{'PASS' if ok else 'FAIL'}**")
    lines.append("")
    lines.append("## Per-frame")
    lines.append("")
    lines.append("| frame | small t/p/e | medium t/p/e | large t/p/e | total | rej_small | rej_big |")
    lines.append("|---|---|---|---|---|---|---|")
    for r in results:
        cells = " | ".join(f"{r['truth'][b]}/{r['pred'][b]}/{r['err'][b]}" for b in BUCKETS)
        lines.append(f"| {r['file']} | {cells} | {r['total_regions']} | "
                     f"{r['rejected_small']} | {r['rejected_big']} |")
    lines.append("")
    lines.append("## New tools shakedown")
    lines.append("")
    lines.append("### Manifest discovery via cmd:list_plugins")
    for name, man in manifest_seen_in_list_plugins.items():
        if man:
            params = [p["name"] for p in man.get("params", [])]
            lines.append(f"- `{name}`: manifest present; params = {params}")
        else:
            lines.append(f"- `{name}`: NO manifest field in cmd:list_plugins reply")
    lines.append("")
    lines.append("### image_pool_stats")
    lines.append("")
    lines.append("Midpoint (after frame 7):")
    lines.append("```json")
    lines.append(json.dumps(midpoint_pool, indent=2)[:4000])
    lines.append("```")
    lines.append("End-of-run:")
    lines.append("```json")
    lines.append(json.dumps(final_pool, indent=2)[:4000])
    lines.append("```")
    lines.append("")
    lines.append("### recent_errors")
    lines.append("")
    lines.append("Midpoint:")
    lines.append("```json")
    lines.append(json.dumps(midpoint_errors, indent=2)[:4000])
    lines.append("```")
    lines.append("End-of-run:")
    lines.append("```json")
    lines.append(json.dumps(final_errors, indent=2)[:4000])
    lines.append("```")
    lines.append("")
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
