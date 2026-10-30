#!/usr/bin/env python3
"""Print NVN coverage the way a human wants to read it.

`docs/NVN_COVERAGE.md` is the generated, authoritative table.  This script is
the terminal view: what is done, what the next useful thing to implement is,
and whether the first real draw call is unblocked.  No side effects, no writes.

    python scripts/nvn_status.py            # summary + next up
    python scripts/nvn_status.py --all      # every domain
"""
from __future__ import annotations

import argparse
import json
import os
import re

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Ordered by "what unblocks the first real draw call" (docs/IOS_PORTING.md, section 10).
PRIORITY = [
    ("P0", "nvnCommandBufferSetRenderTargets"),
    ("P0", "nvnCommandBufferBindProgram"),
    ("P0", "nvnProgramInitialize"),
    ("P0", "nvnCommandBufferBindVertexBuffer"),
    ("P0", "nvnCommandBufferBindVertexBuffers"),
    ("P0", "nvnCommandBufferBindVertexAttribState"),
    ("P1", "nvnCommandBufferDrawElements"),
    ("P1", "nvnCommandBufferBindTexture"),
    ("P1", "nvnCommandBufferBindTextures"),
    ("P1", "nvnTextureInitialize"),
    ("P1", "nvnSamplerInitialize"),
    ("P1", "nvnCommandBufferBindSeparateSampler"),
    ("P2", "nvnCommandBufferBarrier"),
    ("P2", "nvnSyncInitialize"),
    ("P2", "nvnEventInitialize"),
]


def bar(done: int, total: int, width: int = 24) -> str:
    if total <= 0:
        return " " * width
    filled = int(round(done * width / total))
    return "#" * filled + "." * (width - filled)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="list every domain")
    args = ap.parse_args()

    with open(os.path.join(REPO, "ios/nvn/registry.json"), encoding="utf-8") as fh:
        reg = json.load(fh)
    procs = reg["procs"]
    known = {p["name"] for p in procs}

    impl: set[str] = set()
    path = os.path.join(REPO, "ios/nvn/implemented_metal.txt")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip()
                if line:
                    impl.add(line)

    # A name only counts if it is also really bound: a stale txt file must not
    # inflate the number.  verify_ios.py enforces the same rule; this is the
    # read-only view of it.
    mm = os.path.join(REPO, "ios/nvn/src/nvn_metal.mm")
    bound: set[str] = set()
    if os.path.exists(mm):
        with open(mm, encoding="utf-8") as fh:
            bound = set(re.findall(r'BIND\(\s*"(nvn[A-Za-z0-9_]+)"', fh.read()))
    effective = impl & bound

    total = len(procs)
    done = len(effective)
    print(f"NVN host-HLE  {done}/{total}  ({done * 100.0 / total:.1f}%)")
    print(f"  [{bar(done, total)}]")
    if impl - bound:
        print(f"  ! declared but not bound: {sorted(impl - bound)[:5]}")
    if bound - impl:
        print(f"  ! bound but not declared: {sorted(bound - impl)[:5]}")

    domains: dict[str, list[str]] = {}
    for p in procs:
        domains.setdefault(p.get("domain") or "?", []).append(p["name"])

    rows = []
    for name, names in domains.items():
        d = sum(1 for n in names if n in effective)
        rows.append((d / len(names), d, len(names), name))
    rows.sort(key=lambda r: (-r[0], -r[2]))

    shown = rows if args.all else rows[:10]
    print("\nby domain")
    for _ratio, d, n, name in shown:
        print(f"  {name:<20} {d:>3}/{n:<3} [{bar(d, n, 12)}]")
    if not args.all and len(rows) > len(shown):
        print(f"  ... {len(rows) - len(shown)} more domains (--all)")

    print("\nnext up (docs/IOS_PORTING.md, section 10)")
    for pri, name in PRIORITY:
        if name not in known:
            print(f"  {pri}  {name:<40} (not in registry)")
            continue
        mark = "done" if name in effective else "TODO"
        print(f"  {pri}  {name:<40} {mark}")

    p0 = [n for pri, n in PRIORITY if pri == "P0" and n in known]
    p0_open = [n for n in p0 if n not in effective]
    print(f"\nP0 blockers open: {len(p0_open)}/{len(p0)}")
    for n in p0_open:
        print(f"  - {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())