#!/usr/bin/env python3
"""Report the facts a guest-core bring-up needs for a 1.6.0 NSO module.

Counts are over the module's text segment only (the flat image's first
`text` bytes).  No title data is written anywhere; the tool only prints.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "guest" / "third_party" / "nxnative"))
import nso  # type: ignore  # vendored, MIT

SVC_MASK, SVC_WORD = 0xFFE0001F, 0xD4000001
TLS = {
    "mrs tpidr_el0":    (0xFFFFFFE0, 0xD53BD040),
    "mrs tpidrro_el0":  (0xFFFFFFE0, 0xD53BD060),
    "msr tpidr_el0":    (0xFFFFFFE0, 0xD51BD040),
}


def count_words(text: bytes) -> dict[str, int]:
    c = {k: 0 for k in ("svc", *TLS)}
    n = len(text) // 4
    for i in range(n):
        w = struct.unpack_from("<I", text, i * 4)[0]
        if (w & SVC_MASK) == SVC_WORD:
            c["svc"] += 1
            continue
        for name, (mask, want) in TLS.items():
            if (w & mask) == want:
                c[name] += 1
                break
    return c


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("modules", nargs="+", type=Path)
    ap.add_argument("--x18", action="store_true",
                    help="also count x18/w18 operands (needs capstone; slow on main)")
    args = ap.parse_args(argv)
    rc = 0
    for path in args.modules:
        try:
            m = nso.load(path.read_bytes(), path.name)
        except Exception as e:
            print(f"{path}: {e}", file=sys.stderr)
            rc = 1
            continue
        text_end = m.segments[0][1]
        counts = count_words(bytes(m.image[:text_end]))
        print(f"{path.name}: build {m.build_id[:0x10].hex()} text={text_end:#x} "
              f"ro={m.segments[1][1]:#x} data={m.segments[2][1]:#x} bss={m.bss:#x}")
        print("  " + "  ".join(f"{k}={v}" for k, v in counts.items()))
        if args.x18:
            try:
                from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
            except ImportError:
                print("  x18: capstone not installed, skipped", file=sys.stderr)
                continue
            import re
            md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
            md.skipdata = True
            n = sum(1 for ins in md.disasm(bytes(m.image[:text_end]), 0)
                    if ins.mnemonic != ".byte" and re.search(r"\b[xw]18\b", ins.op_str))
            print(f"  x18={n}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
