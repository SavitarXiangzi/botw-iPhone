#!/usr/bin/env python3
"""Extract the NVN entry-point surface a title actually asks for.

Why this exists
---------------
`nvnDeviceGetProcAddress` resolves entry points *by name at runtime*: the names
live in the game's own read-only data as plain ASCII.  That makes the real API
surface observable without any symbol table, and it makes the port's coverage
auditable: the registry below is the exact set of strings the title can ever
hand to the resolver.

Input is whatever binary holds those strings (the main NSO / its decompressed
form).  No game data is copied into the output -- only API names, which are
public header identifiers.

Usage
-----
    python extract_nvn_registry.py --input main.bin \
        --out ../ios/nvn/registry.json \
        --proto path/to/m3_nvn.py

`--proto` is optional; when given, each entry is annotated with whether the
Windows Unicorn prototype already emulates it, so the JSON doubles as a
porting scoreboard.
"""
from __future__ import annotations

import argparse
import json
import re
import sys

# Longest first: "MemoryPoolBuilder" must win over "MemoryPool".
DOMAINS = [
    "VertexAttribState", "VertexStreamState", "ViewportState", "DepthStencilState",
    "ChannelMaskState", "MultisampleState", "PolygonState", "BlendState", "ColorState",
    "MemoryPoolBuilder", "MemoryPool", "TextureBuilder", "TexturePool", "Texture",
    "SamplerBuilder", "SamplerPool", "Sampler",
    "BufferBuilder", "Buffer", "WindowBuilder", "Window",
    "QueueBuilder", "Queue", "DeviceBuilder", "Device",
    "CommandBuffer", "Program", "Sync", "Event", "Bootstrap",
]

TOKEN_RE = re.compile(rb"nvn[A-Z][A-Za-z0-9_]{2,64}")


def scan(path: str) -> list[str]:
    with open(path, "rb") as fh:
        blob = fh.read()
    names: set[str] = set()
    for m in TOKEN_RE.finditer(blob):
        end = m.end()
        # Reject a match that is a prefix of a longer identifier: the byte after
        # it must not continue the token.
        if end < len(blob):
            nxt = blob[end:end + 1]
            if nxt.isalnum() or nxt == b"_":
                continue
        names.add(m.group().decode("ascii"))
    return sorted(names)


def domain_of(name: str) -> str:
    rest = name[3:]
    for d in DOMAINS:
        if rest.startswith(d):
            return d
    return "Other"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="binary containing the nvn* name strings")
    ap.add_argument("--out", required=True, help="registry.json to write")
    ap.add_argument("--proto", help="optional prototype source to cross-annotate")
    args = ap.parse_args(argv)

    names = scan(args.input)
    if not names:
        print("error: no nvn* strings found; wrong input?", file=sys.stderr)
        return 2

    proto_src = ""
    if args.proto:
        try:
            with open(args.proto, encoding="utf-8", errors="replace") as fh:
                proto_src = fh.read()
        except OSError as exc:
            print(f"warning: --proto unreadable ({exc})", file=sys.stderr)

    procs = []
    for n in names:
        entry = {"name": n, "domain": domain_of(n)}
        if proto_src:
            entry["prototype"] = n in proto_src
        entry["metal"] = False
        procs.append(entry)

    counts: dict[str, int] = {}
    for p in procs:
        counts[p["domain"]] = counts.get(p["domain"], 0) + 1

    doc = {
        "schema": 1,
        "source": "runtime-resolved nvn* name strings in title binary",
        "count": len(procs),
        "domains": dict(sorted(counts.items(), key=lambda kv: -kv[1])),
        "procs": procs,
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1, sort_keys=False)
        fh.write("\n")

    print(f"wrote {args.out}: {len(procs)} procs across {len(counts)} domains")
    for d, c in doc["domains"].items():
        print(f"  {d:<20} {c:>3}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
