#!/usr/bin/env python3
"""Rewrite the four Breath of the Wild 1.6.0 NSO modules for native ARM64.

This is the build-time half of the guest core: the vendored nxnative rewriter
turns retail NSO code into an image whose x18 uses, TLS-register accesses,
SVCs, counters and spin loops go through host gates.  The output is still
guest code; no game data is copied into this repository.

Usage:
    python scripts/guest/rewrite_botw.py --nso-dir <ExeFS/update> --out <dir>

Inputs are the user's own 1.6.0 files: main, sdk, subsdk0, rtld (NSO0).
The script refuses any other build ID before it writes anything.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "scripts" / "guest" / "third_party" / "nxnative"
PROFILES = REPO / "scripts" / "guest" / "profiles"
VERIFY = REPO / "scripts" / "guest" / "verify_rewrite.py"

# Full 0x20-byte build IDs are stored in the NSO header at 0x40.  The rewriter
# keys on the first 16 bytes; compare all 32 hex characters so a different
# revision with a shared prefix is rejected too.
BUILDS = {
    "main":    "8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000",
    "sdk":     "fcf93a41b08c5193a1410d262602dc621617c56f000000000000000000000000",
    "subsdk0": "93ff8252d975adf07230cb86c83301c53f2804df000000000000000000000000",
    "rtld":    "39e10c07096696c9c4e228496c4bc44d6b9e0041000000000000000000000000",
}


def fail(msg: str) -> "NoReturn":
    print(f"rewrite_botw: {msg}", file=sys.stderr)
    raise SystemExit(1)


def read_build_id(path: Path) -> str:
    with path.open("rb") as f:
        head = f.read(0x60)
    if len(head) < 0x60 or head[:4] != b"NSO0":
        fail(f"{path}: not an NSO0 file")
    return head[0x40:0x60].hex()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nso-dir", required=True, type=Path,
                    help="directory holding the retail main/sdk/subsdk0/rtld NSO files")
    ap.add_argument("--out", required=True, type=Path,
                    help="output directory for *.patched.bin and tables")
    ap.add_argument("--modules", nargs="+", default=list(BUILDS),
                    choices=list(BUILDS), help="modules to rewrite (default: all four)")
    ap.add_argument("--skip-verify", action="store_true",
                    help="write the images but do not run verify_rewrite.py")
    args = ap.parse_args(argv)

    nso_dir: Path = args.nso_dir
    out: Path = args.out
    missing = [m for m in args.modules if not (nso_dir / m).is_file()]
    if missing:
        fail(f"missing NSO file(s) in {nso_dir}: {', '.join(missing)}")
    for m in args.modules:
        got = read_build_id(nso_dir / m)
        want = BUILDS[m]
        if got != want:
            fail(f"{m}: build ID {got} is not the 1.6.0 build {want}")

    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["DM_GAMES_DIR"] = str(PROFILES)
    env["DM_GAME"] = "01007EF00011E000"

    for m in args.modules:
        print(f"== {m} ==", flush=True)
        cmd = [sys.executable, str(TOOLS / "patch_nso.py"),
               str(nso_dir / m), str(out), m]
        rc = subprocess.call(cmd, env=env)
        if rc != 0:
            fail(f"{m}: patch_nso.py exited {rc}")
        if not args.skip_verify:
            cmd = [sys.executable, str(VERIFY), str(out), m]
            rc = subprocess.call(cmd)
            if rc != 0:
                fail(f"{m}: verify_rewrite.py exited {rc}")
    print("rewrite_botw: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
