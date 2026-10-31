#!/usr/bin/env python3
"""Offline relocation probe for a rewritten 1.6.0 module image.

Reads only artefacts produced by scripts/guest/rewrite_botw.py.  It never
executes guest code and prints nothing that identifies title content beyond
symbol names already present in the module's own ELF tables.

    python scripts/guest/probe_relocs.py build/guest/main.patched.bin \
        --fixups build/guest/main.fixups.tsv

Why this exists: core/guest/guest_image.c used to assume every R_ABS64 was a
base-relative fixup (sym == 0).  The 1.6.0 modules prove that wrong -- main and
sdk carry R_ABS64 entries whose symbol index points at imported C++ RTTI
symbols (__cxa_pure_virtual and friends).  A loader that writes base + addend
for those silently produces wild vtable pointers, so the probe reports the
symbol histogram and refuses to call such an image "fine".

Exit codes: 0 = layout is self-consistent, 1 = the loader would refuse or
produce a wrong image, 2 = usage/IO error.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys

DT_NULL = 0
DT_PLTRELSZ = 2
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_INIT = 12
DT_PLTREL = 20
DT_JMPREL = 23

R_NONE = 0
R_ABS64 = 257
R_GLOB_DAT = 1025
R_JUMP_SLOT = 1026
R_RELATIVE = 1027
TLS_TYPES = {1028, 1029, 1030, 1031, 1032, 1033, 1034}

TYPE_NAMES = {
    R_NONE: "R_NONE",
    R_ABS64: "R_ABS64",
    R_GLOB_DAT: "R_GLOB_DAT",
    R_JUMP_SLOT: "R_JUMP_SLOT",
    R_RELATIVE: "R_RELATIVE",
}
for _t in sorted(TLS_TYPES):
    TYPE_NAMES[_t] = "R_TLS_%d" % _t

FIXUP_KEYS = (
    "PATCH_OFF",
    "KEY_PAGE_OFF",
    "KEY_PAGE_SIZE",
    "TEXT_END",
    "RO_OFF",
    "DATA_OFF",
    "BUILD_ID",
)
KEY_PAGE_SIZE = 0x4000
ELF_RELA_SIZE = 24
ELF_SYM_SIZE = 24


class ProbeError(Exception):
    pass


def read_fixups(path):
    """Parse the '# KEY<TAB>value' header of a fixups sidecar."""
    fields = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.startswith("#"):
                continue
            body = line[1:].strip()
            if not body:
                continue
            parts = body.replace("\t", " ").split(None, 1)
            if len(parts) != 2:
                continue
            key, value = parts[0], parts[1].strip()
            if key == "BUILD_ID":
                fields[key] = value
            else:
                try:
                    fields[key] = int(value, 0)
                except ValueError:
                    raise ProbeError("fixups: %s is not an integer: %r" % (key, value))
    missing = [k for k in FIXUP_KEYS if k not in fields]
    if missing:
        raise ProbeError("fixups: missing %s" % ", ".join(missing))
    return fields


def check_fixups_layout(fx, file_len):
    """Mirror the ordering checks in botw_guest_image_open()."""
    problems = []
    if fx["KEY_PAGE_SIZE"] != KEY_PAGE_SIZE:
        problems.append("KEY_PAGE_SIZE is %#x, loader expects %#x"
                        % (fx["KEY_PAGE_SIZE"], KEY_PAGE_SIZE))
    if not (0 < fx["TEXT_END"] <= fx["RO_OFF"] <= fx["DATA_OFF"] <= fx["KEY_PAGE_OFF"]):
        problems.append("text/ro/data/key are out of order")
    if fx["KEY_PAGE_OFF"] + fx["KEY_PAGE_SIZE"] > fx["PATCH_OFF"]:
        problems.append("key page overlaps the patch area")
    if fx["PATCH_OFF"] >= file_len:
        problems.append("PATCH_OFF leaves no patch bytes")
    return problems


def find_mod0(data):
    if len(data) < 16:
        raise ProbeError("image is too small for a MOD0 header")
    mod0 = struct.unpack_from("<I", data, 4)[0]
    if mod0 + 8 > len(data) or data[mod0:mod0 + 4] != b"MOD0":
        raise ProbeError("MOD0 not found at %#x" % mod0)
    delta = struct.unpack_from("<i", data, mod0 + 4)[0]
    dyn = mod0 + delta
    if dyn < 0 or dyn + 16 > len(data):
        raise ProbeError("dynamic table at %#x is outside the image" % dyn)
    return mod0, dyn


def read_dynamic(data, dyn):
    out = {}
    for i in range(4096):
        at = dyn + i * 16
        if at + 16 > len(data):
            raise ProbeError("dynamic table is not terminated inside the image")
        tag, val = struct.unpack_from("<QQ", data, at)
        if tag == DT_NULL:
            return out
        out.setdefault(tag, []).append(val)
    raise ProbeError("dynamic table has no DT_NULL")


def first(dyn, tag, default=0):
    return dyn.get(tag, [default])[0]


class Report:
    def __init__(self):
        self.total = 0
        self.by_type = {}
        self.abs64_symbols = {}
        self.abs64_undef = 0
        self.abs64_local = 0
        self.abs64_abs = 0
        self.abs64_undef_names = {}
        self.abs64_local_names = {}
        self.sym0_imports = 0
        self.got_undef = 0
        self.got_local = 0
        self.import_names = {}
        self.out_of_bounds = 0
        self.text_relocs = 0
        self.abs64_addend_min = None
        self.abs64_addend_max = None
        self.relative_addend_min = None
        self.relative_addend_max = None
        self.tls = 0
        self.unknown = []

    def to_dict(self):
        return {
            "total": self.total,
            "by_type": {TYPE_NAMES.get(k, hex(k)): v for k, v in sorted(self.by_type.items())},
            "abs64_with_symbol": sum(self.abs64_symbols.values()) - self.abs64_symbols.get(0, 0),
            "abs64_undefined": self.abs64_undef,
            "abs64_module_local": self.abs64_local,
            "abs64_absolute": self.abs64_abs,
            "sym0_imports": self.sym0_imports,
            "got_undefined": self.got_undef,
            "got_module_local": self.got_local,
            "imports": len(self.import_names),
            "out_of_bounds": self.out_of_bounds,
            "text_relocs": self.text_relocs,
            "tls": self.tls,
            "unknown": sorted({hex(t) for t in self.unknown}),
        }


def analyse(data, fx, collect_names=True):
    mod0, dyn = find_mod0(data)
    info = read_dynamic(data, dyn)
    symtab = first(info, DT_SYMTAB)
    strtab = first(info, DT_STRTAB)
    strsz = first(info, DT_STRSZ)
    rela_ent = first(info, DT_RELAENT, ELF_RELA_SIZE)
    syment = first(info, DT_SYMENT, ELF_SYM_SIZE)
    if rela_ent < ELF_RELA_SIZE or syment < ELF_SYM_SIZE:
        raise ProbeError("ELF entry sizes are too small (rela %d, sym %d)" % (rela_ent, syment))
    if not symtab or not strtab or not strsz:
        raise ProbeError("dynamic table has no symbol/string table")
    if strtab + strsz > len(data):
        raise ProbeError("string table runs past the end of the image")

    def symbol(index):
        """(st_shndx, st_value, name) for a dynamic symbol index."""
        at = symtab + index * syment
        if at + ELF_SYM_SIZE > len(data):
            return None, 0, None
        st_name = struct.unpack_from("<I", data, at)[0]
        st_shndx = struct.unpack_from("<H", data, at + 6)[0]
        st_value = struct.unpack_from("<Q", data, at + 8)[0]
        if st_name >= strsz:
            return st_shndx, st_value, None
        start = strtab + st_name
        end = data.find(b"\0", start, strtab + strsz)
        if end < 0:
            return st_shndx, st_value, None
        return st_shndx, st_value, data[start:end].decode("utf-8", "replace")

    rep = Report()
    tables = []
    if first(info, DT_RELA) and first(info, DT_RELASZ):
        tables.append(("rela", first(info, DT_RELA), first(info, DT_RELASZ), rela_ent))
    if first(info, DT_JMPREL) and first(info, DT_PLTRELSZ):
        if first(info, DT_PLTREL, 7) != 7:
            raise ProbeError("DT_PLTREL is not RELA (only RELA is supported)")
        tables.append(("jmprel", first(info, DT_JMPREL), first(info, DT_PLTRELSZ), rela_ent))

    for table, off, size, ent in tables:
        if off > len(data) or len(data) - off < size:
            raise ProbeError("%s table %#x+%#x is outside the image" % (table, off, size))
        for i in range(size // ent):
            at = off + i * ent
            r_off, r_info, r_add = struct.unpack_from("<QQq", data, at)
            rtype = r_info & 0xFFFFFFFF
            sym = r_info >> 32
            rep.total += 1
            rep.by_type[rtype] = rep.by_type.get(rtype, 0) + 1
            if r_off + 8 > len(data):
                rep.out_of_bounds += 1
            elif r_off < fx["TEXT_END"]:
                rep.text_relocs += 1
            if rtype == R_ABS64:
                rep.abs64_symbols[sym] = rep.abs64_symbols.get(sym, 0) + 1
                rep.abs64_addend_min = r_add if rep.abs64_addend_min is None else min(rep.abs64_addend_min, r_add)
                rep.abs64_addend_max = r_add if rep.abs64_addend_max is None else max(rep.abs64_addend_max, r_add)
                if sym:
                    shndx, st_value, name = symbol(sym)
                    if shndx == 0:
                        rep.abs64_undef += 1
                        if collect_names:
                            rep.abs64_undef_names[name or "<sym %d>" % sym] = rep.abs64_undef_names.get(name or "<sym %d>" % sym, 0) + 1
                    elif shndx == 0xFFF1:
                        rep.abs64_abs += 1
                    else:
                        rep.abs64_local += 1
                        if collect_names:
                            rep.abs64_local_names[name or "<sym %d>" % sym] = rep.abs64_local_names.get(name or "<sym %d>" % sym, 0) + 1
            elif rtype == R_RELATIVE:
                rep.relative_addend_min = r_add if rep.relative_addend_min is None else min(rep.relative_addend_min, r_add)
                rep.relative_addend_max = r_add if rep.relative_addend_max is None else max(rep.relative_addend_max, r_add)
            elif rtype in (R_GLOB_DAT, R_JUMP_SLOT):
                if sym == 0:
                    rep.sym0_imports += 1
                    continue
                shndx, _, name = symbol(sym)
                if shndx == 0:
                    rep.got_undef += 1
                    rep.import_names[name or "<sym %d>" % sym] = True
                else:
                    rep.got_local += 1
            elif rtype in TLS_TYPES:
                rep.tls += 1
            elif rtype != R_NONE:
                rep.unknown.append(rtype)
    rep.dyn_init = first(info, DT_INIT)
    rep.mod0 = mod0
    rep.dyn = dyn
    return rep


def describe(path, fx_path, as_json=False):
    data = open(path, "rb").read()
    fx = read_fixups(fx_path)
    problems = check_fixups_layout(fx, len(data))
    rep = analyse(data, fx)
    named = {TYPE_NAMES.get(k, hex(k)): v for k, v in sorted(rep.by_type.items())}
    if as_json:
        out = rep.to_dict()
        out["problems"] = problems
        out["init"] = "0x%x" % rep.dyn_init
        out["build_id"] = fx["BUILD_ID"]
        print(json.dumps(out, indent=2, sort_keys=True))
        return 0 if not problems else 1

    print("image      %s" % path)
    print("build id   %s" % fx["BUILD_ID"])
    print("size       %#x  (text ends %#x, data %#x, key page %#x+%#x, patch %#x)"
          % (len(data), fx["TEXT_END"], fx["DATA_OFF"], fx["KEY_PAGE_OFF"],
             fx["KEY_PAGE_SIZE"], fx["PATCH_OFF"]))
    print("mod0       %#x  dyn %#x  init %#x" % (rep.mod0, rep.dyn, rep.dyn_init))
    print("relocations %d: %s" % (rep.total, ", ".join("%s=%d" % kv for kv in named.items())))

    def summary(counter, keep=5):
        items = sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))
        head = ", ".join("%s x%d" % (name, count) for name, count in items[:keep])
        if len(items) > keep:
            head += ", ... (%d more)" % (len(items) - keep)
        return head

    with_sym = sum(rep.abs64_symbols.values()) - rep.abs64_symbols.get(0, 0)
    if rep.abs64_symbols:
        print("R_ABS64    addends %#x..%#x, %d entr(ies), %d carry a symbol"
              % (rep.abs64_addend_min, rep.abs64_addend_max, sum(rep.abs64_symbols.values()), with_sym))
        print("           undefined %d (S = HLE symbol), module-local %d (S = base + st_value), absolute %d"
              % (rep.abs64_undef, rep.abs64_local, rep.abs64_abs))
        if rep.abs64_undef_names:
            print("           undef:  %s" % summary(rep.abs64_undef_names))
        if rep.abs64_local_names:
            print("           local:  %s" % summary(rep.abs64_local_names))
    if rep.relative_addend_min is not None:
        print("R_RELATIVE addends %#x..%#x" % (rep.relative_addend_min, rep.relative_addend_max))
    print("GOT/PLT    %d undefined (HLE) + %d module-local"
          % (rep.got_undef, rep.got_local))
    print("imports    %d unique HLE names" % len(rep.import_names))
    print("in text    %d relocation(s) target the text segment" % rep.text_relocs)
    if rep.sym0_imports:
        print("BAD        %d R_GLOB_DAT/R_JUMP_SLOT entries have no symbol" % rep.sym0_imports)
    if rep.tls:
        print("UNSUPPORTED %d TLS relocation(s)" % rep.tls)
    if rep.unknown:
        print("UNSUPPORTED unknown relocation types %s" % sorted({hex(t) for t in rep.unknown}))
    if rep.out_of_bounds:
        print("BAD        %d relocation(s) point outside the image" % rep.out_of_bounds)
    for problem in problems:
        print("BAD        %s" % problem)
    ok = (not problems and not rep.out_of_bounds and not rep.tls
          and not rep.unknown and not rep.sym0_imports)
    print("verdict    %s" % ("layout ok" if ok else "loader would refuse this image"))
    return 0 if ok else 1


def build_self_test_image(abs64_symbol=1, abs64_offset=0x808, sym_shndx=0, sym_value=0):
    """A synthetic flat image exercising every relocation the loader applies."""
    size = 0x9000
    data = bytearray(size)
    # MOD0 sits above the dynamic table so the two do not overlap.
    struct.pack_into("<I", data, 4, 0x180)
    data[0x180:0x184] = b"MOD0"
    struct.pack_into("<i", data, 0x184, 0x80 - 0x180)
    dyn = 0x80
    entries = [
        (DT_RELA, 0x200), (DT_RELASZ, ELF_RELA_SIZE * 3), (DT_RELAENT, ELF_RELA_SIZE),
        (DT_JMPREL, 0x248), (DT_PLTRELSZ, ELF_RELA_SIZE), (DT_PLTREL, 7),
        (DT_SYMTAB, 0x300), (DT_SYMENT, ELF_SYM_SIZE),
        (DT_STRTAB, 0x400), (DT_STRSZ, 0x40),
        (DT_INIT, 0x1000), (DT_NULL, 0),
    ]
    for i, (tag, val) in enumerate(entries):
        struct.pack_into("<QQ", data, dyn + i * 16, tag, val)
    struct.pack_into("<I", data, 0x300 + ELF_SYM_SIZE, 1)
    struct.pack_into("<H", data, 0x300 + ELF_SYM_SIZE + 6, sym_shndx)
    struct.pack_into("<Q", data, 0x300 + ELF_SYM_SIZE + 8, sym_value)
    data[0x400:0x400 + 10] = b"\0nvn_test\0"
    relas = [
        (0x800, 0, R_RELATIVE, 0x1234),
        (abs64_offset, abs64_symbol, R_ABS64, 0x10),
        (0x810, 1, R_JUMP_SLOT, 0),
    ]
    for i, (r_off, sym, rtype, addend) in enumerate(relas):
        struct.pack_into("<QQq", data, 0x200 + i * ELF_RELA_SIZE, r_off, (sym << 32) | rtype, addend)
    struct.pack_into("<QQq", data, 0x248, 0x818, (1 << 32) | R_GLOB_DAT, 0)
    fixups = (
        "# PATCH_OFF\t0x8000\n"
        "# KEY_PAGE_OFF\t0x4000\n"
        "# KEY_PAGE_SIZE\t0x4000\n"
        "# TEXT_END\t0x1000\n"
        "# RO_OFF\t0x1000\n"
        "# DATA_OFF\t0x2000\n"
        "# BUILD_ID\t00112233445566778899aabbccddeeff\n"
    )
    return bytes(data), fixups


def self_test():
    data, fixups_text = build_self_test_image()
    fx = {}
    for line in fixups_text.splitlines():
        key, value = [part.strip() for part in line[1:].split("\t", 1)]
        fx[key] = value if key == "BUILD_ID" else int(value, 0)
    rep = analyse(data, fx)
    assert rep.total == 4, rep.total
    assert rep.by_type == {R_RELATIVE: 1, R_ABS64: 1, R_JUMP_SLOT: 1, R_GLOB_DAT: 1}, rep.by_type
    assert rep.abs64_symbols == {1: 1}, rep.abs64_symbols
    assert rep.import_names == {"nvn_test": True}, rep.import_names
    assert rep.out_of_bounds == 0 and rep.tls == 0 and not rep.unknown
    assert rep.dyn_init == 0x1000, rep.dyn_init
    assert (rep.abs64_undef, rep.abs64_local) == (1, 0), (rep.abs64_undef, rep.abs64_local)
    assert rep.abs64_undef_names == {"nvn_test": 1}, rep.abs64_undef_names
    assert not check_fixups_layout(fx, len(data))

    # A module-local ABS64 symbol resolves to base + st_value, never to HLE.
    local, _ = build_self_test_image(sym_shndx=1, sym_value=0x500)
    rep_local = analyse(local, fx)
    assert (rep_local.abs64_undef, rep_local.abs64_local) == (0, 1), rep_local.to_dict()
    assert rep_local.abs64_local_names == {"nvn_test": 1}

    # A base-relative ABS64 (sym 0) must be reported as such.
    data0, _ = build_self_test_image(abs64_symbol=0)
    assert analyse(data0, fx).abs64_symbols == {0: 1}

    # Out-of-bounds relocation targets must be counted, not silently accepted.
    bad, _ = build_self_test_image(abs64_offset=0x8FFC)
    assert analyse(bad, fx).out_of_bounds == 1

    # Layout violations must be reported.
    broken = dict(fx)
    broken["KEY_PAGE_SIZE"] = 0x400
    assert check_fixups_layout(broken, len(data))
    broken = dict(fx)
    broken["PATCH_OFF"] = 0x9000
    assert check_fixups_layout(broken, len(data))
    print("probe_relocs: self-test ok (rela, jmprel, abs64 symbols, bounds, layout)")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Inspect a rewritten guest image's relocations.")
    ap.add_argument("image", nargs="?", help="path to <module>.patched.bin")
    ap.add_argument("--fixups", help="path to <module>.fixups.tsv")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--self-test", action="store_true", help="run the built-in synthetic checks")
    args = ap.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if not args.image or not args.fixups:
        ap.error("image and --fixups are required (or pass --self-test)")
    try:
        return describe(args.image, args.fixups, args.json)
    except (OSError, ProbeError) as exc:
        print("probe_relocs: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
