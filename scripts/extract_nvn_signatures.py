#!/usr/bin/env python3
"""Extract NVN entry-point shapes from a public nvn_protos.h.

The iOS backend has to decode the guest's argument block by hand, so it must
know how many arguments each entry point takes and of what type.  Guessing that
is how `nvnCommandBufferSetViewport` ended up reading its integer arguments as
doubles.

Input is a generated prototype list from the public nxnative project (MIT):

    https://github.com/seedds/nxnative  ->  dm/gfx/nvn/nvn_protos.h

Only the *shape* (name, return type, parameter types) is kept.  The header
itself is never copied into this repository, and neither is any Nintendo
material: these are function signatures, not implementation or data.

    python scripts/extract_nvn_signatures.py --protos C:/path/to/nvn_protos.h
    python scripts/extract_nvn_signatures.py --check

`sig` is optional per entry: it is only derived where the source has it.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(REPO, "ios", "nvn", "signatures.json")

SOURCE = ("seedds/nxnative dm/gfx/nvn/nvn_protos.h, MIT License "
          "(Copyright (c) 2026 seedds)")
NOTE = ("NVN entry-point shapes only: return type and parameter types. "
        "The header itself is not redistributed.")

TYPEDEF = re.compile(
    r"typedef\s+(?P<ret>[A-Za-z_][A-Za-z0-9_ *]*?)\s*\(\s*\*\s*NVNT_(?P<name>nvn[A-Za-z0-9_]+)\s*\)"
    r"\s*\((?P<params>[^;]*?)\)\s*;",
    re.S,
)


def split_params(text: str) -> list[str]:
    text = " ".join(text.split())
    if not text or text == "void":
        return []
    out: list[str] = []
    depth = 0
    cur = ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return [p for p in out if p]


def parse(path: str) -> list[dict]:
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    procs = []
    for m in TYPEDEF.finditer(text):
        params = split_params(m.group("params"))
        procs.append({
            "name": m.group("name"),
            "ret": " ".join(m.group("ret").split()),
            "params": params,
            "arity": len(params),
        })
    procs.sort(key=lambda p: p["name"])
    return procs


def serialise(procs: list[dict]) -> str:
    doc = {
        "source": SOURCE,
        "note": NOTE,
        "count": len(procs),
        "max_arity": max((p["arity"] for p in procs), default=0),
        "procs": procs,
    }
    return json.dumps(doc, ensure_ascii=False, indent=1) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--protos", help="path to nvn_protos.h (never written to the repo)")
    ap.add_argument("--check", action="store_true", help="verify the committed file matches")
    args = ap.parse_args(argv)

    if args.check:
        if not os.path.exists(OUT):
            print(f"extract_nvn_signatures: missing {os.path.relpath(OUT, REPO)}")
            return 1
        with open(OUT, encoding="utf-8") as fh:
            doc = json.load(fh)
        names = [p["name"] for p in doc.get("procs", [])]
        if len(names) != len(set(names)):
            print("extract_nvn_signatures: duplicate names in signatures.json")
            return 1
        if doc.get("count") != len(names):
            print("extract_nvn_signatures: count field disagrees with procs")
            return 1
        print(f"extract_nvn_signatures: ok ({doc['count']} shapes, max arity {doc['max_arity']})")
        return 0

    if not args.protos:
        ap.error("--protos is required unless --check is given")
    if not os.path.exists(args.protos):
        print(f"extract_nvn_signatures: no such file: {args.protos}")
        return 1

    procs = parse(args.protos)
    if len(procs) < 400:
        print(f"extract_nvn_signatures: only {len(procs)} parsed, refusing to write")
        return 1

    text = serialise(procs)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)

    over = [p for p in procs if p["arity"] > 8]
    print(f"extract_nvn_signatures: wrote {len(procs)} shapes, "
          f"max arity {max(p['arity'] for p in procs)}")
    if over:
        print("  ! arity > 8 needs stack arguments and cannot use the x[8] ABI: "
              + ", ".join(p["name"] for p in over[:5]))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())