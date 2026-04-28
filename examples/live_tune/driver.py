"""live_tune driver — exercises the recompile_project_plugin loop.

Flow:
  1. open_project (cold compile of broken plugin)
  2. compile_and_load (inspect.cpp)
  3. iter 0: run all 10 frames with the BROKEN plugin (erode radius 8)
  4. iter 1: edit erode radius -> 1, recompile_project_plugin, re-run
  5. shakedown: introduce a syntax error, expect ProtocolError +
     diagnostics, verify previous DLL still serves runs
  6. iter 2: write the final converged plugin (erode 5 + bbox-aspect
     splitter for touching-circle pairs), recompile, re-run

Persists driver_log.json + driver_log.txt for RESULTS.md to consume.
"""
from __future__ import annotations

import json
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT       = Path(__file__).resolve().parent
PROJECT    = ROOT
SCRIPT     = ROOT / "inspect.cpp"
PLUGIN_SRC = ROOT / "plugins" / "circle_counter" / "src" / "plugin.cpp"
FRAMES     = sorted((ROOT / "frames").glob("frame_*.png"))

# ---- Plugin source variants -----------------------------------------

BROKEN_SRC = r"""// circle_counter — DELIBERATELY MIS-TUNED initial plugin.
// Goal: agent edits this file and calls cmd:recompile_project_plugin
// in a loop until every frame reports count == 8.
//
// Hint of what's wrong: the erosion radius is way too aggressive for
// the smaller circles in the dataset, so they vanish before
// findFilledRegions sees them. Either reduce the erosion radius or
// drop erosion entirely. The local-contrast pass is fine.

#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>

using namespace xi::ops;

class CircleCounter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        auto src = input.get_image("src");
        if (src.empty()) return xi::Record().set("count", 0);

        // Local-contrast mask.
        auto blurred = gaussian(src, 2);
        auto bg      = boxBlur(blurred, 40);

        xi::Image mask(src.width, src.height, 1);
        const uint8_t* sp = blurred.data();
        const uint8_t* bp = bg.data();
        uint8_t* dp = mask.data();
        int n = src.width * src.height;
        for (int i = 0; i < n; ++i) {
            int diff = (int)bp[i] - (int)sp[i];
            dp[i] = diff > 18 ? 255 : 0;
        }

        // MIS-TUNED — radius=8 erodes away most of the small circles.
        auto cleaned = erode(mask, 8);

        auto regions = findFilledRegions(cleaned);
        int count = 0;
        for (auto& r : regions) {
            int area = (int)r.size();
            if (area >= 50 && area <= 5000) ++count;
        }

        return xi::Record()
            .image("mask", cleaned)
            .set("count", count);
    }
};

XI_PLUGIN_IMPL(CircleCounter)
"""

ITER1_SRC = BROKEN_SRC.replace("erode(mask, 8)", "erode(mask, 1)")

BAD_SRC = ITER1_SRC.replace("return xi::Record()", "return xi::Recordd()", 1)

FINAL_SRC = r"""// circle_counter — tuned via cmd:recompile_project_plugin loop.
//
// Erosion radius reduced from 8 (which destroyed r=11 circles) to 5,
// which preserves all singles after the contrast threshold.
//
// Dataset wrinkle: generate.py places circles with no overlap check,
// so some frames have two touching circles that survive erosion as one
// figure-8 region. Counting raw regions therefore under-counts those
// frames. Fix: for each region compute its bounding-box aspect ratio
// (longest/shortest side). A single circle has aspect ~1; a pair of
// touching circles has aspect ~2. We round aspect to the nearest int
// (>=1) and credit that many singles per region. Circle-radius
// variation (11..17) keeps singles well under aspect 1.5.

#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>

#include <algorithm>
#include <climits>
#include <vector>

using namespace xi::ops;

class CircleCounter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        auto src = input.get_image("src");
        if (src.empty()) return xi::Record().set("count", 0);

        // Local-contrast mask.
        auto blurred = gaussian(src, 2);
        auto bg      = boxBlur(blurred, 40);

        xi::Image mask(src.width, src.height, 1);
        const uint8_t* sp = blurred.data();
        const uint8_t* bp = bg.data();
        uint8_t* dp = mask.data();
        int n = src.width * src.height;
        for (int i = 0; i < n; ++i) {
            int diff = (int)bp[i] - (int)sp[i];
            dp[i] = diff > 18 ? 255 : 0;
        }

        // Light erosion to clean speckle without devouring r=11 blobs.
        auto cleaned = erode(mask, 5);

        auto regions = findFilledRegions(cleaned);

        // For each region in the area band, count singles via bbox aspect.
        int count = 0;
        for (auto& r : regions) {
            int area = (int)r.size();
            if (area < 50 || area > 5000) continue;
            int xmin = INT_MAX, xmax = INT_MIN, ymin = INT_MAX, ymax = INT_MIN;
            for (auto& p : r) {
                if (p.x < xmin) xmin = p.x;
                if (p.x > xmax) xmax = p.x;
                if (p.y < ymin) ymin = p.y;
                if (p.y > ymax) ymax = p.y;
            }
            int w = xmax - xmin + 1;
            int h = ymax - ymin + 1;
            int lo = std::min(w, h);
            int hi = std::max(w, h);
            int n_in_region = (hi + lo / 2) / lo;  // round(hi/lo)
            if (n_in_region < 1) n_in_region = 1;
            count += n_in_region;
        }

        return xi::Record()
            .image("mask", cleaned)
            .set("count", count);
    }
};

XI_PLUGIN_IMPL(CircleCounter)
"""

# ---- helpers --------------------------------------------------------

def write_src(text: str) -> None:
    PLUGIN_SRC.write_text(text, encoding="utf-8")


def run_all(c: Client) -> list[int]:
    out = []
    for fp in FRAMES:
        r = c.run(frame_path=str(fp))
        if r.value("error"):
            out.append(-1)
        else:
            out.append(int(r.value("count", -1)))
    return out


def fmt_table(label: str, counts: list[int]) -> str:
    cells = " ".join(f"{c:>3}" for c in counts)
    ok    = sum(1 for c in counts if c == 8)
    return f"{label:<24s} | {cells} | {ok}/{len(counts)} == 8"


def main():
    log_lines: list[str] = []
    iteration_log: list[dict] = []

    def log(msg: str):
        print(msg, flush=True)
        log_lines.append(msg)

    # Reset plugin source to broken state before opening project.
    write_src(BROKEN_SRC)

    with Client() as c:
        log_buf: list[dict] = []
        c.on_log(lambda m: log_buf.append(m))

        log(f"open_project: {PROJECT}")
        op = c.open_project(str(PROJECT))
        log(f"  plugins: {[p['name'] for p in op.get('plugins', [])]}")
        log(f"  instances: {[i['name'] for i in op.get('instances', [])]}")

        log(f"compile_and_load: {SCRIPT}")
        c.compile_and_load(str(SCRIPT))

        # ---- Iteration 0 — broken (erode 8) ----
        log("\n=== iter 0: BROKEN (erode radius 8) ===")
        counts = run_all(c)
        log(fmt_table("iter 0 (erode 8)", counts))
        iteration_log.append({
            "iter": 0, "change": "baseline (erode radius 8)",
            "reattached": None, "diagnostics": None,
            "counts": counts,
        })

        # ---- Iteration 1 — erode 1 ----
        log("\n=== iter 1: edit erode 8 -> 1, recompile_project_plugin ===")
        write_src(ITER1_SRC)
        log_buf.clear()
        rcp = c.recompile_project_plugin("circle_counter")
        log(f"  reply: plugin={rcp.get('plugin')!r} "
            f"reattached={rcp.get('reattached')} "
            f"diagnostics_count={len(rcp.get('diagnostics') or [])}")
        counts = run_all(c)
        log(fmt_table("iter 1 (erode 1)", counts))
        iteration_log.append({
            "iter": 1, "change": "erode radius 8 -> 1",
            "reattached": rcp.get("reattached"),
            "diagnostics": rcp.get("diagnostics"),
            "counts": counts,
        })

        # ---- Shakedown — broken source must surface diagnostics ----
        log("\n=== shakedown: deliberate syntax error ===")
        write_src(BAD_SRC)
        log_buf.clear()
        shakedown_err = None
        shakedown_log_excerpt = None
        try:
            c.recompile_project_plugin("circle_counter")
            log("  UNEXPECTED: recompile succeeded with broken source")
        except ProtocolError as e:
            shakedown_err = str(e)
            log(f"  ProtocolError as expected: {shakedown_err[:120]}")
            err_lines = [m["msg"] for m in log_buf if m.get("level") == "error"]
            shakedown_log_excerpt = "\n".join(err_lines[-15:])
            for ln in err_lines[-5:]:
                log(f"    log> {ln}")

        counts_during_break = run_all(c)
        log(fmt_table("during break", counts_during_break))
        log(f"  -> iter-1 DLL still attached (counts unchanged): "
            f"{counts_during_break == counts}")

        # ---- Iteration 2 — final (erode 5 + bbox-aspect splitter) ----
        log("\n=== iter 2: final tune (erode 5 + bbox-aspect splitter) ===")
        write_src(FINAL_SRC)
        log_buf.clear()
        rcp2 = c.recompile_project_plugin("circle_counter")
        log(f"  reply: plugin={rcp2.get('plugin')!r} "
            f"reattached={rcp2.get('reattached')} "
            f"diagnostics_count={len(rcp2.get('diagnostics') or [])}")
        counts2 = run_all(c)
        log(fmt_table("iter 2 (final)", counts2))
        iteration_log.append({
            "iter": 2, "change": "erode radius 5 + bbox-aspect splitter",
            "reattached": rcp2.get("reattached"),
            "diagnostics": rcp2.get("diagnostics"),
            "counts": counts2,
        })

        ok = all(c8 == 8 for c8 in counts2)
        log(f"\nconverged: {ok}")

        out = {
            "iterations": iteration_log,
            "final_counts": counts2,
            "converged": ok,
            "shakedown": {
                "protocol_error": shakedown_err,
                "log_excerpt": shakedown_log_excerpt,
                "counts_during_break": counts_during_break,
                "old_dll_still_attached": counts_during_break == counts,
            },
        }
        (ROOT / "driver_log.json").write_text(json.dumps(out, indent=2))
        (ROOT / "driver_log.txt").write_text("\n".join(log_lines))


if __name__ == "__main__":
    main()
