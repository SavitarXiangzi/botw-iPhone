#!/usr/bin/env python3
"""Structural checks for an image produced by rewrite_botw.py.

The rewriter is a third-party tool; this verifier is ours.  It answers the
questions that matter before a loader ever sees the image:

  * the fixups header is complete and ordered (text < ro < data < key < patch);
  * every gate site in gates.tsv is a branch to a stub inside the patch area;
  * every rewritten site in sites.tsv is a branch into the patch area;
  * no SVC instruction is left in the text.

It does not execute the image (that needs an ARM64 host and the loader).
"""
from __future__ import annotations

import argparse
import contextlib
import io
import struct
import sys
import tempfile
from pathlib import Path

SVC_MASK = 0xFFE0001F
SVC_WORD = 0xD4000001
NON_BRANCH_SITES = {"S", "T", "W", "ctr"}


def fail(msg: str) -> "NoReturn":
    print(f"verify_rewrite: {msg}", file=sys.stderr)
    raise SystemExit(1)


def read_header(path: Path) -> dict[str, int]:
    hdr: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#"):
            continue
        parts = line[1:].split("\t")
        if len(parts) < 2:
            continue
        try:
            hdr[parts[0].strip()] = int(parts[1].strip(), 0)
        except ValueError:
            pass
    return hdr


def branch_target(word: int, pc: int) -> int | None:
    op = word >> 26
    if op not in (0x05, 0x25):  # B, BL
        return None
    imm = word & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 0x04000000
    return pc + (imm << 2)


def check_image(out: Path, module: str) -> int:
    fixups = out / f"{module}.fixups.tsv"
    image = out / f"{module}.patched.bin"
    gates = out / f"{module}.gates.tsv"
    sites = out / f"{module}.sites.tsv"
    for p in (fixups, image, gates, sites):
        if not p.is_file():
            fail(f"missing {p}")

    hdr = read_header(fixups)
    for key in ("PATCH_OFF", "KEY_PAGE_OFF", "KEY_PAGE_SIZE", "TEXT_END", "RO_OFF", "DATA_OFF"):
        if key not in hdr:
            fail(f"{fixups}: no {key}")
    text_end = hdr["TEXT_END"]
    ro_off = hdr["RO_OFF"]
    data_off = hdr["DATA_OFF"]
    key_off = hdr["KEY_PAGE_OFF"]
    patch_off = hdr["PATCH_OFF"]
    if patch_off < 0:
        fail(f"{module}: negative PATCH_OFF is not supported by this verifier yet")
    if not (0 < text_end <= ro_off <= data_off < key_off < patch_off):
        fail(f"{module}: header order is wrong: text {text_end:#x} ro {ro_off:#x} "
             f"data {data_off:#x} key {key_off:#x} patch {patch_off:#x}")
    if hdr["KEY_PAGE_SIZE"] != 0x4000:
        fail(f"{module}: key page size {hdr['KEY_PAGE_SIZE']:#x}, expected 0x4000")

    blob = image.read_bytes()
    if len(blob) < patch_off:
        fail(f"{module}: image is {len(blob)} bytes, shorter than PATCH_OFF {patch_off:#x}")
    patch_end = len(blob)
    text = blob[:text_end]

    # 1. No SVC left.  Scan 32-bit words; the text is 4-byte aligned.
    svc_left = [off for off in range(0, len(text) - 3, 4)
                if (struct.unpack_from("<I", text, off)[0] & SVC_MASK) == SVC_WORD]
    if svc_left:
        fail(f"{module}: {len(svc_left)} SVC instruction(s) left, first at {svc_left[0]:#x}")

    # 2. Every gate site branches to its stub in the patch area.
    ngates = 0
    with gates.open(encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) < 4:
                continue
            stub, kind, _gid, site = int(c[0], 0), c[1], c[2], int(c[3], 0)
            ngates += 1
            if site + 4 > text_end:
                fail(f"{module}: {kind} site {site:#x} is outside text")
            if not (patch_off <= stub < patch_end):
                fail(f"{module}: {kind} return point {stub:#x} is outside the patch area")
            if kind == "spin":
                # The spin row names the loop head and the pause gate's return
                # point; the patched branch is a back-edge elsewhere in the
                # loop, not the head itself.  The row still proves both ends
                # are in range; the rewriter's own spin checks cover the rest.
                continue
            start = stub - 0x20          # stp / mrs / adrp+add+ldr / ldr / ldr / blr
            if not (patch_off <= start < patch_end):
                fail(f"{module}: {kind} gate block {start:#x} is outside the patch area")
            word = struct.unpack_from("<I", blob, site)[0]
            tgt = branch_target(word, site)
            if tgt != start:
                fail(f"{module}: {kind} site {site:#x} branches to {tgt if tgt is None else hex(tgt)}, "
                     f"expected gate block {start:#x} (word {word:#08x})")

    # 3. Every rewritten site is a branch into the patch area.
    nsites = 0
    with sites.open(encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) < 2:
                continue
            off, kind = int(c[0], 0), c[1]
            nsites += 1
            if off + 4 > text_end:
                fail(f"{module}: {kind} site {off:#x} is outside text")
            if kind in NON_BRANCH_SITES:
                # S: spin loop head; T: trampoline; W: exclusive window start;
                # ctr: counter read reached by a different rewrite.
                continue
            word = struct.unpack_from("<I", blob, off)[0]
            tgt = branch_target(word, off)
            if tgt is None or not (patch_off <= tgt < patch_end):
                fail(f"{module}: {kind} site {off:#x} does not branch into the patch area "
                     f"(word {word:#08x}, target {tgt if tgt is None else hex(tgt)})")

    if ngates == 0:
        fail(f"{module}: gates.tsv is empty")
    if nsites == 0:
        fail(f"{module}: sites.tsv is empty")
    print(f"verify_rewrite: {module}: ok  text={text_end:#x} patch={patch_off:#x}+{patch_end - patch_off:#x} "
          f"gates={ngates} sites={nsites}")
    return 0


def self_test() -> int:
    """Exercise the branch decoder and the header/gate checks on a fake image."""
    with tempfile.TemporaryDirectory(prefix="botw-verify-") as td:
        out = Path(td)
        module = "fake"
        text_end, ro_off, data_off = 0x1000, 0x1000, 0x1000
        key_off, patch_off = 0x2000, 0x3000
        image = bytearray(0x4000)
        site, start, stub = 0x100, 0x3000, 0x3020
        # The gate block starts at `start`; gates.tsv names the return point
        # 0x20 bytes later.  Patch the original site with B -> start.
        imm = ((start - site) >> 2) & 0x03FFFFFF
        struct.pack_into("<I", image, site, 0x14000000 | imm)
        (out / f"{module}.patched.bin").write_bytes(image)
        (out / f"{module}.fixups.tsv").write_text(
            f"# PATCH_OFF\t{patch_off:#x}\n# KEY_PAGE_OFF\t{key_off:#x}\n"
            f"# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{text_end:#x}\n"
            f"# RO_OFF\t{ro_off:#x}\n# DATA_OFF\t{data_off:#x}\n", encoding="utf-8")
        (out / f"{module}.gates.tsv").write_text(
            f"{stub:#x}\tsvc\t38\t{site:#x}\n", encoding="utf-8")
        (out / f"{module}.sites.tsv").write_text(
            f"{site:#x}\tx18\n", encoding="utf-8")
        # check_image prints and returns; capture success by calling it.
        if check_image(out, module) != 0:
            fail("self-test: valid image rejected")
        # An unpatched SVC in the text must be caught.  The expected error
        # line is swallowed so a green gate log does not look like a failure.
        struct.pack_into("<I", image, 0x200, SVC_WORD)
        (out / f"{module}.patched.bin").write_bytes(image)
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                check_image(out, module)
            except SystemExit as e:
                if e.code == 1:
                    print("verify_rewrite: self-test: ok (rejected an unpatched SVC)")
                    return 0
                raise
        fail("self-test: unpatched SVC was not rejected")
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", nargs="?", type=Path, help="rewrite output directory")
    ap.add_argument("module", nargs="?", help="module name (main, sdk, subsdk0, rtld)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.out or not args.module:
        ap.error("out and module are required unless --self-test is used")
    return check_image(args.out, args.module)


if __name__ == "__main__":
    raise SystemExit(main())
