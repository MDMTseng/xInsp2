"""Single-frame debug: dump VARs and intermediate images."""
import json
from pathlib import Path
from xinsp2 import Client, dump_run

ROOT = Path(__file__).parent
INSPECT_CPP = ROOT / "inspect.cpp"

with Client() as c:
    c.compile_and_load(str(INSPECT_CPP))
    c.set_param("frame_idx", 0)
    run = c.run()
    print("vars:")
    for v in run.vars:
        kind = v.get("kind")
        if kind == "image":
            print(f"  {v['name']}: image gid={v.get('gid')} {v.get('width')}x{v.get('height')}")
        else:
            print(f"  {v['name']} ({kind}) = {v.get('value')}")
    out = ROOT / "snapshots" / "run_debug"
    out.mkdir(parents=True, exist_ok=True)
    dump_run(run, str(out))
    print(f"dumped to {out}")
