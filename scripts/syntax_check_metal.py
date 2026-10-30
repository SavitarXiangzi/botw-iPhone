#!/usr/bin/env python3
"""Type-check ios/nvn/src/nvn_metal.mm without a Mac.

nvn_metal.mm is Objective-C++ and includes <Metal/Metal.h> and
<QuartzCore/CAMetalLayer.h>, so a plain C++ compiler cannot touch it.  With
clang and the stub headers in tests/stubs/ it can be *type-checked* on any
host: `-fsyntax-only` parses, resolves every message send against the stub
protocols and instantiates the templates, but emits no code and needs no
Apple SDK.

This is not a substitute for `xcodebuild`.  It will not catch: missing
framework linkage, availability annotations, ARC semantics, or anything the
stubs model more loosely than the real SDK.  It does catch the class of bug
that actually bit this file -- passing the wrong pointer type into a handle.

    python scripts/syntax_check_metal.py            # skip if no clang
    python scripts/syntax_check_metal.py --require  # fail if no clang
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

CANDIDATES = [
    os.environ.get("CLANGXX", ""),
    r"C:\Program Files\LLVM\bin\clang++.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe",
    "/usr/bin/clang++",
    "/usr/local/opt/llvm/bin/clang++",
]


def find_clang() -> str | None:
    for c in CANDIDATES:
        if c and os.path.isfile(c):
            return c
    return shutil.which("clang++") or shutil.which("clang")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--require", action="store_true", help="fail instead of skipping when clang is absent")
    args = ap.parse_args()

    clang = find_clang()
    if not clang:
        msg = "syntax_check_metal: SKIP (no clang++; install LLVM or pass CLANGXX=...)"
        print(msg)
        return 1 if args.require else 0

    # Relative paths on purpose: the repo may live under a directory whose name
    # is outside the local console codepage, and clang echoes paths verbatim.
    cmd = [
        clang,
        "-fsyntax-only",
        "-x", "objective-c++",
        "-std=c++20",
        "-fblocks",
        # The real target is ARC + the modern runtime.  Without these clang
        # falls back to the GNUstep/fragile ABI, where Objective-C subscripting
        # (rpd.colorAttachments[0]) is not even parsed.
        "-fobjc-arc",
        "-fobjc-runtime=ios-17.0",
        "-Wall", "-Wextra", "-Werror",
        "-D__APPLE__=1",
        "-I", "tests/stubs",
        "-I", "ios/nvn/include",
        "ios/nvn/src/nvn_metal.mm",
    ]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True)
    # Never let a console codepage mismatch turn a compile error into a crash.
    raw = proc.stdout + proc.stderr
    try:
        out = raw.decode("utf-8", errors="replace").strip()
    except Exception:  # pragma: no cover - decode with replace cannot fail
        out = repr(raw)
    if proc.returncode != 0:
        print(out)
        print("syntax_check_metal: FAIL")
        return 1
    print(f"syntax_check_metal: ok ({os.path.basename(clang)})")
    if out:
        print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())