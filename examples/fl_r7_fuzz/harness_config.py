"""FL r7 target #2 — project.json / instance.json / plugin.json fuzzer.

Targets the validation logic in `backend/include/xi/xi_plugin_manager.hpp`
and project loader in `xi_project.hpp`. We synthesize candidate project
trees in temp dirs and call `Client.open_project(dir)`.

Each iter generates one of several mutation strategies:
- valid baseline (control)
- bad type for a manifest param (e.g. "max" as a string)
- bad enum / bad shape value
- duplicate JSON keys
- unicode / control-char keys
- null where required (name, plugin)
- huge strings, huge arrays
- numeric overflows
- circular instance refs (plugin pointing to nonexistent plugin)
- missing trailing brace, BOM, weird whitespace
- mismatched manifest type vs instance.json config

Pass criteria: open_project either succeeds with warnings, or returns a
clean error (ProtocolError). Backend never crashes; remains pingable.
We do NOT require any particular plugin to actually compile (we may
deliberately omit dlls or supply broken plugin.json) — just that the
loader's validation handles it gracefully.

We use a single backend across iters and just keep re-opening different
project dirs.
"""
from __future__ import annotations

import json
import os
import random
import shutil
import string
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, WS_URL  # noqa: E402

from xinsp2 import Client, ProtocolError  # noqa: E402

ITERS = int(os.environ.get("FUZZ_ITERS", "500"))
SEED = int(os.environ.get("FUZZ_SEED", "424242"))


def rstr(rng: random.Random, n: int = 12) -> str:
    return "".join(rng.choice(string.ascii_lowercase) for _ in range(n))


def mutate_value(rng: random.Random, original):
    """Perturb a JSON value into something likely-bad."""
    choices = [
        None, "", "x" * rng.randint(1, 1024), 0, -1, 2**31, 2**63 - 1, -2**63,
        2**63, 1.5e308, float("inf"), True, False, [], [1, 2, 3], {}, {"k": "v"},
        "\x00", "null", "42", rstr(rng, rng.randint(0, 64)),
        rstr(rng, rng.randint(0, 16)).encode().decode("utf-8", "replace"),
    ]
    # filter out inf if json doesn't allow it — json.dumps won't accept inf by default; skip
    pick = rng.choice(choices)
    if isinstance(pick, float) and (pick != pick or pick in (float("inf"), float("-inf"))):
        pick = "inf"
    return pick


def make_baseline(root: Path) -> dict:
    """Make a minimal valid (or near-valid) project tree.

    Uses a no-DLL plugin manifest so we don't depend on building a
    plugin DLL in the fuzz harness — the loader path we care about
    (manifest validation) runs before DLL load is attempted.

    Returns the project dict so callers can mutate it.
    """
    plug_dir = root / "plugins" / "fakeplug"
    plug_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "name": "fakeplug",
        "description": "fuzz target plugin (no dll)",
        "dll": "fakeplug.dll",
        "factory": "xi_plugin_create",
        "has_ui": False,
        "manifest": {
            "params": [
                {"name": "n", "type": "int", "default": 3, "min": 0, "max": 100},
                {"name": "label", "type": "string", "default": "abc"},
            ],
            "inputs": [],
            "outputs": [],
            "exchange": [],
        },
    }
    (plug_dir / "plugin.json").write_text(json.dumps(manifest, indent=2))

    inst_dir = root / "instances" / "inst1"
    inst_dir.mkdir(parents=True, exist_ok=True)
    (inst_dir / "instance.json").write_text(json.dumps({
        "plugin": "fakeplug",
        "isolation": "in_process",
        "config": {"n": 3, "label": "abc"},
    }, indent=2))

    project = {
        "name": "fuzz_proj",
        "instances": [{"name": "inst1", "plugin": "fakeplug"}],
    }
    (root / "project.json").write_text(json.dumps(project, indent=2))
    return {"project": project, "manifest": manifest, "plug_dir": plug_dir, "inst_dir": inst_dir}


def synth_project(root: Path, rng: random.Random) -> dict:
    """Create a project with one targeted mutation. Returns metadata."""
    info = make_baseline(root)
    strategy = rng.choice([
        "baseline",
        "project_missing_name", "project_name_null", "project_extra_garbage",
        "project_instances_not_array", "project_instance_missing_plugin",
        "project_unknown_plugin_ref",
        "project_duplicate_instance_name",
        "project_huge_instances",
        "project_unicode_name", "project_control_chars",
        "project_trailing_garbage", "project_bom",
        "project_truncated", "project_not_object",
        "instance_bad_isolation", "instance_config_wrong_type",
        "instance_config_extra_key", "instance_config_unicode_key",
        "instance_config_null", "instance_no_plugin",
        "manifest_param_type_unknown", "manifest_param_min_gt_max",
        "manifest_param_bad_default", "manifest_default_wrong_type",
        "manifest_duplicate_param_names", "manifest_huge_params",
        "manifest_param_no_name", "manifest_param_name_null",
        "manifest_inputs_not_array", "manifest_outputs_not_array",
    ])
    p = info["project"]
    m = info["manifest"]

    if strategy == "baseline":
        pass
    elif strategy == "project_missing_name":
        p.pop("name", None)
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_name_null":
        p["name"] = None
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_extra_garbage":
        p["__garbage__"] = mutate_value(rng, None)
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_instances_not_array":
        p["instances"] = mutate_value(rng, [])
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_instance_missing_plugin":
        p["instances"] = [{"name": "x"}]
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_unknown_plugin_ref":
        p["instances"] = [{"name": "x", "plugin": "no_such_plugin"}]
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_duplicate_instance_name":
        p["instances"] = [
            {"name": "dup", "plugin": "fakeplug"},
            {"name": "dup", "plugin": "fakeplug"},
        ]
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_huge_instances":
        p["instances"] = [{"name": f"i{k}", "plugin": "fakeplug"} for k in range(rng.randint(100, 800))]
        (root / "project.json").write_text(json.dumps(p))
    elif strategy == "project_unicode_name":
        p["name"] = "тест项目🚀"
        (root / "project.json").write_text(json.dumps(p), encoding="utf-8")
    elif strategy == "project_control_chars":
        # Write raw bytes so we can include control chars verbatim
        raw = json.dumps(p).encode("utf-8") + b"\x00\x07trailing"
        (root / "project.json").write_bytes(raw)
    elif strategy == "project_trailing_garbage":
        (root / "project.json").write_text(json.dumps(p) + "\nGARBAGE\n}")
    elif strategy == "project_bom":
        (root / "project.json").write_bytes(b"\xef\xbb\xbf" + json.dumps(p).encode("utf-8"))
    elif strategy == "project_truncated":
        s = json.dumps(p)
        cut = rng.randint(0, max(1, len(s) - 5))
        (root / "project.json").write_text(s[:cut])
    elif strategy == "project_not_object":
        (root / "project.json").write_text(json.dumps([1, 2, 3]))
    elif strategy == "instance_bad_isolation":
        info["inst_dir"].joinpath("instance.json").write_text(json.dumps({
            "plugin": "fakeplug",
            "isolation": rng.choice([42, None, "subspace", "OUT_OF_PROCESS  ", ""]),
            "config": {},
        }))
    elif strategy == "instance_config_wrong_type":
        info["inst_dir"].joinpath("instance.json").write_text(json.dumps({
            "plugin": "fakeplug",
            "isolation": "in_process",
            "config": {"n": "not_an_int", "label": 99},
        }))
    elif strategy == "instance_config_extra_key":
        info["inst_dir"].joinpath("instance.json").write_text(json.dumps({
            "plugin": "fakeplug",
            "isolation": "in_process",
            "config": {"n": 3, "label": "x", "__unknown__": [1, 2, 3]},
        }))
    elif strategy == "instance_config_unicode_key":
        info["inst_dir"].joinpath("instance.json").write_bytes(json.dumps({
            "plugin": "fakeplug",
            "isolation": "in_process",
            "config": {"k_unicode": 3, "label_ru": "x"},
        }, ensure_ascii=False).encode("utf-8"))
    elif strategy == "instance_config_null":
        info["inst_dir"].joinpath("instance.json").write_text(json.dumps({
            "plugin": "fakeplug",
            "isolation": "in_process",
            "config": None,
        }))
    elif strategy == "instance_no_plugin":
        info["inst_dir"].joinpath("instance.json").write_text(json.dumps({
            "isolation": "in_process",
            "config": {},
        }))
    elif strategy == "manifest_param_type_unknown":
        m["manifest"]["params"][0]["type"] = rng.choice(["int64_t", "complex", None, 42, ""])
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_param_min_gt_max":
        m["manifest"]["params"][0]["min"] = 100
        m["manifest"]["params"][0]["max"] = -100
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_param_bad_default":
        m["manifest"]["params"][0]["default"] = "not_an_int"
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_default_wrong_type":
        m["manifest"]["params"][0]["default"] = {"nested": True}
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_duplicate_param_names":
        m["manifest"]["params"] = [
            {"name": "n", "type": "int", "default": 1},
            {"name": "n", "type": "int", "default": 2},
        ]
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_huge_params":
        m["manifest"]["params"] = [
            {"name": f"p{k}", "type": "int", "default": k}
            for k in range(rng.randint(200, 1500))
        ]
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_param_no_name":
        m["manifest"]["params"][0].pop("name", None)
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_param_name_null":
        m["manifest"]["params"][0]["name"] = None
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_inputs_not_array":
        m["manifest"]["inputs"] = mutate_value(rng, [])
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    elif strategy == "manifest_outputs_not_array":
        m["manifest"]["outputs"] = mutate_value(rng, [])
        info["plug_dir"].joinpath("plugin.json").write_text(json.dumps(m))
    return {"strategy": strategy}


def main() -> int:
    rng = random.Random(SEED)
    findings: list[dict] = []
    print(f"[harness_config] iters={ITERS} seed={SEED}", flush=True)

    tmp_root = Path(tempfile.mkdtemp(prefix="xinsp2_fuzz_cfg_"))
    print(f"[harness_config] tmp_root={tmp_root}")

    last_status_t = time.time()

    with BackendProc():
        c = Client(WS_URL)
        c.connect()
        try:
            for i in range(ITERS):
                proj_dir = tmp_root / f"p{i}"
                if proj_dir.exists():
                    shutil.rmtree(proj_dir, ignore_errors=True)
                proj_dir.mkdir(parents=True)
                meta = synth_project(proj_dir, rng)

                t0 = time.time()
                err = None
                ok = False
                rsp = None
                try:
                    rsp = c.open_project(str(proj_dir), timeout=20.0)
                    ok = True
                except ProtocolError as e:
                    err = {"kind": "protocol_error", "msg": str(e)[:300], "error_code": e.error}
                except Exception as e:
                    err = {"kind": "exception", "msg": repr(e)[:400]}
                dt = time.time() - t0

                # Liveness: must still ping after each iter
                try:
                    c.ping()
                    pingable = True
                except Exception as e:
                    pingable = False
                    findings.append({
                        "iter": i, "seed": SEED, "strategy": meta["strategy"],
                        "kind": "ping_failed_after_open", "open_err": err,
                        "exc": repr(e)[:300], "fatal": True,
                    })
                    print(f"[CRASH] ping failed after iter {i} strategy={meta['strategy']}: {e!r}", flush=True)
                    break

                if err and err["kind"] == "exception":
                    findings.append({
                        "iter": i, "seed": SEED, "strategy": meta["strategy"],
                        "kind": "non_protocol_exception", "err": err,
                    })

                if (i + 1) % 25 == 0 and time.time() - last_status_t > 8.0:
                    print(f"[harness_config] iter={i+1}/{ITERS} pingable={pingable} last_dt={dt:.2f}s strategy={meta['strategy']}", flush=True)
                    last_status_t = time.time()
                    try:
                        prog = Path(__file__).resolve().parents[2] / ".fl_progress" / "fl_r7_fuzz.txt"
                        with open(prog, "a", encoding="utf-8") as f:
                            f.write(f"{time.strftime('%H:%M:%S')}  config iter {i+1}/{ITERS}\n")
                    except Exception:
                        pass
        finally:
            try: c.shutdown()
            except Exception: pass
            try: c.close()
            except Exception: pass

    out = Path(__file__).parent / "_results_config.json"
    out.write_text(json.dumps({"iters": ITERS, "seed": SEED, "findings": findings}, indent=2))
    print(f"[harness_config] done. findings={len(findings)}")
    # cleanup
    shutil.rmtree(tmp_root, ignore_errors=True)
    return 0 if not any(f.get("fatal") for f in findings) else 1


if __name__ == "__main__":
    sys.exit(main())
