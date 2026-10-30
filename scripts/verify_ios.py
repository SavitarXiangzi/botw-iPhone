#!/usr/bin/env python3
"""Cross-check the iOS side without a Mac.

Everything here runs on any host.  It is deliberately opinionated: it fails on
drift rather than warning, because the whole point of a skeleton built without
Xcode is that the invariants are machine-checked.

Checks
------
1. NVN registry  : schema, uniqueness, name shape, count field agreement.
2. Codegen       : ios/nvn/include/nvn_api.h + src/nvn_dispatch.cpp +
                   docs/NVN_COVERAGE.md are exactly what gen_nvn.py produces.
3. Tri-set       : registry ∩ BIND(...) in nvn_metal.mm == implemented_metal.txt.
4. C/C++ lint    : delimiter balance for headers/sources (string- and
                   comment-aware), so a truncated file cannot slip through.
5. Swift/plist   : delimiter balance, @main present, MetalView wired.
6. Enum honesty  : Release builds must not ship while nvn_metal.mm says
                   kEnumsVerified = 0  (checked only with --release).
7. No-leak       : no title data, keys or absolute Windows paths in shipped
                   source directories.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

problems: list[str] = []
notes: list[str] = []


def fail(msg: str) -> None:
    problems.append(msg)


def read(rel: str) -> str:
    with open(os.path.join(REPO, rel), encoding="utf-8", errors="replace") as fh:
        return fh.read()


def rel_exists(rel: str) -> bool:
    return os.path.exists(os.path.join(REPO, rel))


# --------------------------------------------------------------------------- #
# 1. registry
# --------------------------------------------------------------------------- #

def check_registry() -> dict:
    rel = "ios/nvn/registry.json"
    if not rel_exists(rel):
        fail(f"missing {rel}")
        return {}
    doc = json.loads(read(rel))
    procs = doc.get("procs")
    if not isinstance(procs, list) or not procs:
        fail(f"{rel}: no procs")
        return {}
    names = [p["name"] for p in procs]
    if len(names) != len(set(names)):
        dupes = sorted({n for n in names if names.count(n) > 1})
        fail(f"{rel}: duplicate procs {dupes[:5]}")
    if doc.get("count") != len(procs):
        fail(f"{rel}: count={doc.get('count')} but {len(procs)} procs")
    bad = [n for n in names if not re.fullmatch(r"nvn[A-Z][A-Za-z0-9_]*", n)]
    if bad:
        fail(f"{rel}: malformed names {bad[:5]}")
    missing_domain = [p["name"] for p in procs if not p.get("domain")]
    if missing_domain:
        fail(f"{rel}: procs without domain {missing_domain[:5]}")
    return {"names": set(names), "doc": doc}


# --------------------------------------------------------------------------- #
# 2. codegen is up to date
# --------------------------------------------------------------------------- #

def check_codegen() -> None:
    gen = os.path.join(REPO, "scripts", "gen_nvn.py")
    if not os.path.exists(gen):
        fail("missing scripts/gen_nvn.py")
        return
    proc = subprocess.run([sys.executable, gen, "--repo", REPO, "--check"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        fail("generated artifacts are stale:\n" + (proc.stdout + proc.stderr).strip())
    else:
        notes.append(proc.stdout.strip())


# --------------------------------------------------------------------------- #
# 3. registry vs BIND list vs implemented_metal.txt
# --------------------------------------------------------------------------- #

def check_triset(reg: dict) -> None:
    mm = read("ios/nvn/src/nvn_metal.mm") if rel_exists("ios/nvn/src/nvn_metal.mm") else ""
    bound = set(re.findall(r"BIND\(\s*\"(nvn[A-Za-z0-9_]+)\"", mm))
    impl = set()
    if rel_exists("ios/nvn/implemented_metal.txt"):
        for line in read("ios/nvn/implemented_metal.txt").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                impl.add(line)
    else:
        fail("missing ios/nvn/implemented_metal.txt")

    if bound - impl:
        fail(f"bound in nvn_metal.mm but not declared implemented: {sorted(bound - impl)[:5]}")
    if impl - bound:
        fail(f"declared implemented but has no BIND(...): {sorted(impl - bound)[:5]}")
    unknown = bound - reg.get("names", set())
    if unknown:
        fail(f"BIND(...) names absent from the registry: {sorted(unknown)[:5]}")
    notes.append(f"metal bindings: {len(bound)}/{len(reg.get('names', []))}")


# --------------------------------------------------------------------------- #
# 4. delimiter balance
# --------------------------------------------------------------------------- #

PAIRS = {"(": ")", "[": "]", "{": "}"}


def balance(rel: str, text: str) -> None:
    stack: list[tuple[str, int]] = []
    i, n = 0, len(text)
    line = 1
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        if ch in "\"'":
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                elif text[i] == "\n" and quote == "'":
                    break
                i += 1
            i += 1
            continue
        if ch in PAIRS:
            stack.append((ch, line))
        elif ch in ")]}":
            if not stack:
                fail(f"{rel}:{line}: unmatched '{ch}'")
                return
            opener, oline = stack.pop()
            if PAIRS[opener] != ch:
                fail(f"{rel}:{line}: '{ch}' closes '{opener}' from line {oline}")
                return
        i += 1
    if stack:
        opener, oline = stack[0]
        fail(f"{rel}:{oline}: unclosed '{opener}'")


def check_sources() -> None:
    exts = (".h", ".hpp", ".c", ".cc", ".cpp", ".mm")
    roots = ["core", "ios/nvn", "tests"]
    count = 0
    for root in roots:
        base = os.path.join(REPO, root)
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                if not f.endswith(exts):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, REPO).replace("\\", "/")
                balance(rel, open(path, encoding="utf-8", errors="replace").read())
                count += 1
    notes.append(f"balanced {count} C/C++/ObjC++ files")


# --------------------------------------------------------------------------- #
# 5. Swift + plist
# --------------------------------------------------------------------------- #

def check_swift() -> None:
    base = os.path.join(REPO, "ios", "BotW")
    if not os.path.isdir(base):
        fail("missing ios/BotW")
        return
    files = [f for f in os.listdir(base) if f.endswith(".swift")]
    if not files:
        fail("no Swift sources")
        return
    joined = ""
    for f in files:
        text = open(os.path.join(base, f), encoding="utf-8").read()
        joined += text
        balance(f"ios/BotW/{f}", text)
    if "@main" not in joined:
        fail("ios/BotW: no @main entry point")
    if "MetalView" not in joined:
        fail("ios/BotW: MetalView is not wired")
    notes.append(f"swift sources: {len(files)}")


def check_plist() -> None:
    rel = "ios/project.yml"
    if not rel_exists(rel):
        fail(f"missing {rel}")
        return
    text = read(rel)
    for key in ("name:", "targets:", "schemes:", "deploymentTarget"):
        if key not in text:
            fail(f"{rel}: missing '{key}'")
    if "core" not in text:
        fail(f"{rel}: core/ is not part of any target (the C ABI would not build)")
    # Every `- path:` under sources must exist on disk.  An empty directory is
    # enough to make `xcodegen generate` fail, and that only shows up on the
    # mac runner -- so catch it here instead (see docs/IOS_PORTING.md).
    for m in re.finditer(r"^\s*- path:\s*(\S+)\s*$", text, re.M):
        src = m.group(1)
        resolved = os.path.normpath(os.path.join(REPO, "ios", src))
        if not os.path.exists(resolved):
            rel_resolved = os.path.relpath(resolved, REPO).replace("\\", "/")
            fail(f"{rel}: source path '{src}' does not exist ({rel_resolved})")


# --------------------------------------------------------------------------- #
# 6. enum honesty
# --------------------------------------------------------------------------- #

def check_enums(release: bool) -> None:
    rel = "ios/nvn/src/nvn_metal.mm"
    if not rel_exists(rel):
        fail(f"missing {rel}")
        return
    text = read(rel)
    m = re.search(r"constexpr\s+int\s+kEnumsVerified\s*=\s*(\d+)", text)
    if not m:
        fail(f"{rel}: kEnumsVerified gate is missing")
        return
    if m.group(1) == "0":
        msg = "nvn_metal.mm: NVN enum values are still unverified (kEnumsVerified = 0)"
        if release:
            fail(msg + " -- refusing a release build")
        else:
            notes.append(msg + " (fine for a skeleton)")
        if "#error" not in text:
            fail(f"{rel}: kEnumsVerified = 0 but the compile-time #error guard is gone")
    else:
        if "Release build refused" in text:
            fail(f"{rel}: kEnumsVerified is non-zero but the #error release guard is still there")
        notes.append("nvn_metal.mm: enum tables marked verified")


# --------------------------------------------------------------------------- #
# 6b. docs referenced from code actually exist
# --------------------------------------------------------------------------- #

DOC_REF = re.compile(r"docs/[A-Za-z0-9_./-]+\.md")


def check_doc_refs() -> None:
    seen: set[str] = set()
    for root in ("core", "ios", "scripts", "tests"):
        base = os.path.join(REPO, root)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                if not f.endswith((".h", ".hpp", ".c", ".cc", ".cpp", ".mm", ".swift", ".metal", ".py")):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, REPO).replace("\\", "/")
                text = open(path, encoding="utf-8", errors="replace").read()
                for m in DOC_REF.finditer(text):
                    ref = m.group(0)
                    if ref in seen:
                        continue
                    seen.add(ref)
                    if not rel_exists(ref):
                        fail(f"{rel}: references {ref}, which does not exist")
    notes.append(f"doc references: {len(seen)} checked")


# --------------------------------------------------------------------------- #
# 7. no title data / keys / absolute host paths
# --------------------------------------------------------------------------- #

def check_leaks() -> None:
    forbidden_ext = (".xci", ".nsp", ".nca", ".nso", ".kdb", ".keys", ".bin")
    forbidden_names = {"prod.keys", "title.keys"}
    for dirpath, _dirs, files in os.walk(REPO):
        if ".git" in dirpath.split(os.sep):
            continue
        for f in files:
            if f in forbidden_names or f.endswith(forbidden_ext):
                fail(f"forbidden artefact in tree: {os.path.relpath(os.path.join(dirpath, f), REPO)}")
    shipped = ["core", "ios/nvn", "tests"]
    pat = re.compile(r"[A-Za-z]:\\\\|[A-Za-z]:\\(?:botw|Users|Program)")
    for root in shipped:
        base = os.path.join(REPO, root)
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                if not f.endswith((".h", ".hpp", ".c", ".cc", ".cpp", ".mm", ".swift", ".metal")):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, REPO).replace("\\", "/")
                for lineno, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                    if pat.search(line):
                        fail(f"{rel}:{lineno}: absolute host path in shipped source")


# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", action="store_true", help="apply release-only gates")
    args = ap.parse_args(argv)

    reg = check_registry()
    if reg:
        check_codegen()
        check_triset(reg)
    check_sources()
    check_swift()
    check_plist()
    check_enums(args.release)
    check_doc_refs()
    check_leaks()

    for n in notes:
        print("  . " + n)
    if problems:
        print()
        for p in problems:
            print("FAIL " + p)
        print(f"\nverify_ios: {len(problems)} problem(s)")
        return 1
    print("\nverify_ios: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
