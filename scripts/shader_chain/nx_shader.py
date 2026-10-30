#!/usr/bin/env python3
"""Offline Maxwell SASS -> SPIR-V -> MSL for this project.

The Windows/Linux half of the chain in docs/SHADER_CHAIN.md:

  extract      game shader package (.pack/.sbfsha/.ssarc; Yaz0 and SARC are
               peeled automatically) -> one code file per stage + manifest.tsv
  translate    manifest -> job.json -> dmsh_xlate (Ryujinx's translator) ->
               SPIR-V -> spirv-cross -> MSL, with the reference chain's flags
  check-tools  print both tools' versions so a log says which chain ran

The last step -- xcrun metal -> .metallib -- needs macOS and is NOT done here.

Shader key (same one nxnative's shader_map.tsv and this repo's prog_key use):
  sha256(control[0:0x880] + code_area)[0:8] as 16 hex digits, where code_area
  still includes its 0x30 header (the 0x50-byte SPH starts at +0x30).
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys

BNSH_MAGIC = b"BNSH\0\0\0\0"
CTL_MAGIC = b"\x34\x12\x76\x98"

# The binary program's stage pointers, in the order the file stores them.  NOT
# NVN's enum order (vs, ps, gs, hs, ds, cs): reading them in that order swaps
# vertex and pixel shaders without any error.
BNSH_STAGES = ["vs", "hs", "ds", "gs", "ps", "cs"]

# spirv-cross flags, verbatim from the working chain (dmsh_build.py XC).
CROSS_ARGS = ["--msl", "--msl-version", "20300", "--msl-argument-buffers",
              "--msl-texture-buffer-native", "--msl-pad-fragment-output"]
ENTRY = {"vs": "nvngfx_main_vs", "ps": "nvngfx_main_fs"}


def yaz0(s):
    """Yaz0 -> bytes; s unchanged when it is not Yaz0."""
    if s[:4] != b"Yaz0":
        return s
    n = struct.unpack_from(">I", s, 4)[0]
    out, i = bytearray(), 16
    while len(out) < n:
        h = s[i]
        i += 1
        for _ in range(8):
            if len(out) >= n:
                break
            if h & 0x80:
                out.append(s[i])
                i += 1
            else:
                b1, b2 = s[i], s[i + 1]
                i += 2
                dist, ln = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
                if ln:
                    ln += 2
                else:
                    ln = s[i] + 0x12
                    i += 1
                st = len(out) - dist
                if st < 0:
                    raise ValueError("Yaz0 back-reference before the start")
                out += out[st:st + ln] if dist >= ln else (out[st:] * (ln // dist + 1))[:ln]
            h = (h << 1) & 0xFF
    return bytes(out[:n])


def sarc_members(d):
    """(name, bytes) per member.  BOTW's packs put SFAT right after the header
    and data_offset at the data; other SARC writers point data_offset at SFAT."""
    if d[:4] != b"SARC" or len(d) < 0x20:
        return
    hl, _bom, _size, doff = struct.unpack_from("<HHII", d, 4)
    sfat = doff if d[doff:doff + 4] == b"SFAT" else hl
    if d[sfat:sfat + 4] != b"SFAT":
        return
    n = struct.unpack_from("<H", d, sfat + 6)[0]
    nodes = sfat + 12
    names = nodes + 16 * n + 8
    for i in range(n):
        _h, attr, s, e = struct.unpack_from("<IIII", d, nodes + 16 * i)
        nm = (d[names + (attr & 0xFFFFFF) * 4:].split(b"\0")[0].decode("utf-8", "replace")
              if attr >> 24 else "#%d" % i)
        yield nm, d[doff + s:doff + e]


def bnsh_shaders(d, base):
    """(variation, stage, control, code_area) for every stage of a BNSH."""
    nvar, pvar = struct.unpack_from("<IQ", d, base + 0x7C)
    if not 0 < nvar < 100000:
        raise ValueError("implausible BNSH variation count %d" % nvar)
    for v in range(nvar):
        prog = struct.unpack_from("<Q", d, base + pvar + 0x40 * v + 0x10)[0]
        if not prog:
            continue
        for si, st in enumerate(BNSH_STAGES):
            sc = struct.unpack_from("<Q", d, base + prog + 8 + 8 * si)[0]
            if not sc:
                continue
            pctl, pcode, ncode, nctl = struct.unpack_from("<QQII", d, base + sc + 8)
            yield (v, st, d[base + pctl:base + pctl + nctl],
                   d[base + pcode:base + pcode + ncode])


def walk(blob, label, out, depth=0):
    """Peel Yaz0/SARC until BNSH is found; append (v, st, ctl, code, where)."""
    if depth > 8:
        raise ValueError("containers nested too deeply at " + label)
    blob = yaz0(blob)
    if blob[:4] == b"SARC":
        for nm, m in sarc_members(blob):
            walk(m, label + "/" + nm, out, depth + 1)
        return
    pos = 0
    while True:
        base = blob.find(BNSH_MAGIC, pos)
        if base < 0:
            return
        pos = base + 4
        if blob[base + 0x60:base + 0x64] != b"grsc":
            continue
        for rec in bnsh_shaders(blob, base):
            out.append(rec + ("%s#BNSH@%#x#var%d" % (label, base, rec[0]),))


def ctl_len(ctl):
    """Control bytes that take part in the key.

    +0x20 holds the section's own length; use it when it is plausible
    (0x760..0x87f), else the first 0x880.  This is the host's rule
    (nxnative hle_nvn.c keys_hash, ios/nvn/src/nvn_shader_key.c): the same
    three places must agree or every offline blob is orphaned."""
    own = struct.unpack_from("<I", ctl, 0x20)[0] if len(ctl) >= 0x24 else 0
    return own if 0x760 <= own < 0x880 else 0x880


def shader_key(ctl, code):
    h = hashlib.sha256()
    h.update(ctl[:ctl_len(ctl)])
    h.update(code)
    return h.hexdigest()[:16]


def sources(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _d, files in os.walk(p):
                for f in files:
                    if f.lower().endswith((".pack", ".sbfsha", ".bfsha", ".ssarc", ".szs")):
                        yield os.path.join(root, f)
        else:
            yield p


def cmd_extract(a):
    out = a.out
    os.makedirs(os.path.join(out, "code"), exist_ok=True)
    seen, rows, files = set(), [], 0
    for src in sources(a.inputs):
        files += 1
        found = []
        try:
            walk(open(src, "rb").read(), os.path.basename(src), found)
        except Exception as e:          # a malformed container must name itself
            print("[warn] %s: %s" % (src, e), file=sys.stderr)
            continue
        for v, st, ctl, code, where in found:
            if st == "cs" or len(code) < 0x80 or ctl[:4] != CTL_MAGIC:
                continue
            key = shader_key(ctl, code)
            if (key, st) in seen:
                continue
            seen.add((key, st))
            # c1: the constants an indirect jump (BRX) reads its targets from.
            # Without it those shaders translate to the wrong control flow.
            c1o, c1t = struct.unpack_from("<2I", ctl, 0x700)
            c1 = code[c1o:c1t] if c1o <= c1t <= len(code) else b""
            path = os.path.join("code", "%s_%s.bin" % (key, st))
            with open(os.path.join(out, path), "wb") as f:
                f.write(code[0x30:])     # translator input: SPH + instructions
            if a.keep_full_code:
                # The key hashes the *whole* code area, 0x30 head included;
                # keycheck.exe needs it to replay the runtime's hash.
                with open(os.path.join(out, "code", "%s_%s.code" % (key, st)), "wb") as f:
                    f.write(code)
            with open(os.path.join(out, path[:-4] + ".ctl"), "wb") as f:
                f.write(ctl)
            rows.append((key, st, path, c1.hex(), where))
    rows.sort(key=lambda r: (r[1], r[0]))
    man = os.path.join(out, "manifest.tsv")
    with open(man, "w", encoding="utf-8") as f:
        f.write("# key\tstage\tcode\tc1\twhere\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    n = {"vs": 0, "ps": 0}
    for r in rows:
        n[r[1]] += 1
    print("extract: %d file(s), %d shader(s): %d vs, %d ps -> %s"
          % (files, len(rows), n["vs"], n["ps"], man))


def tool_cmd(path):
    if not path.lower().endswith(".dll"):
        return [path]
    dotnet = os.environ.get("DOTNET_EXE") or shutil.which("dotnet")
    if not dotnet and os.environ.get("DOTNET_ROOT"):
        root = os.environ["DOTNET_ROOT"]
        dotnet = os.path.join(root, "dotnet.exe" if os.name == "nt" else "dotnet")
    if not dotnet:
        sys.exit("[RED] %s is a .NET assembly and no dotnet was found "
                 "(set DOTNET_ROOT or pass a native dmsh_xlate)" % path)
    return [dotnet, path]


def read_manifest(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            rows.append((line.rstrip("\n").split("\t") + [""] * 5)[:5])
    return rows


def precise(msl):
    # Maxwell's FMNMX returns the other operand for NaN and saturates to 0;
    # Metal's fast::min/max/clamp leave that to the GPU.  precise:: matches.
    return re.sub(r"\bfast::(min|max|clamp)\(", r"precise::\1(", msl)


def rename_entry(msl, stage):
    """spirv-cross emits main0; the backend looks the entry point up by name."""
    return re.sub(r"\bmain0\b", ENTRY[stage], msl) if stage in ENTRY else msl


def cmd_translate(a):
    rows = read_manifest(os.path.join(a.work, "manifest.tsv"))
    if a.keys:
        want = set(a.keys)
        rows = [r for r in rows if r[0] in want]
    if a.limit:
        rows = rows[:a.limit]
    if not rows:
        sys.exit("[RED] no shader selected")
    os.makedirs(a.out, exist_ok=True)
    tmp = os.path.join(a.out, "spv")
    os.makedirs(tmp, exist_ok=True)
    jobs = []
    for key, st, code, c1, _where in rows:
        j = {"key": "%s_%s" % (key, st), "stage": st, "code": os.path.join(a.work, code)}
        if c1:
            j["c1"] = c1
        jobs.append(j)
    job_path = os.path.join(a.out, "job.json")
    with open(job_path, "w", encoding="utf-8") as f:
        json.dump(jobs, f, indent=1)
    rx = subprocess.run(tool_cmd(a.xlate) + [job_path, tmp], capture_output=True, text=True)
    if rx.stdout:
        print(rx.stdout.rstrip())
    if rx.returncode:
        sys.exit("[RED] dmsh_xlate failed (%d): %s" % (rx.returncode, rx.stderr.strip()[:2000]))

    ok = 0
    for j in jobs:
        spv = os.path.join(tmp, j["key"] + ".spv")
        if not os.path.exists(spv):
            print("[warn] no SPIR-V for %s" % j["key"], file=sys.stderr)
            continue
        r = subprocess.run([a.spirv_cross, spv] + CROSS_ARGS, capture_output=True, text=True)
        if r.returncode:
            print("[warn] spirv-cross failed for %s: %s" % (j["key"], r.stderr.strip()[:400]),
                  file=sys.stderr)
            continue
        base = os.path.join(a.out, j["key"])
        msl = rename_entry(precise(r.stdout), j["stage"])
        with open(base + ".metal", "w", encoding="utf-8") as f:
            f.write(msl)
        # c1 is a pointer in the MSL argument buffer, not inline data.  Only
        # write a sidecar when this stage declares the member: an empty file
        # means "explicit empty c1", while a missing one means the shader never
        # reads c1.  Drop a stale sidecar from an earlier run.
        if re.search(r"\b[a-z]+_c1\b", msl):
            with open(base + ".c1", "wb") as f:
                f.write(bytes.fromhex(j.get("c1", "")))
        elif os.path.exists(base + ".c1"):
            os.remove(base + ".c1")
        res = os.path.join(tmp, j["key"] + ".res.json")
        if os.path.exists(res):
            meta = json.load(open(res, encoding="utf-8"))
            meta["entry"], meta["chain"] = ENTRY.get(j["stage"], "main0"), "nx_shader.py"
            with open(base + ".json", "w", encoding="utf-8") as f:
                json.dump(meta, f, indent=1)
            bad = [n for n in meta.get("notes", [])
                   if "Invalid instruction" in n or "not implemented" in n]
            if bad:
                print("[warn] %s: %d translator note(s), first: %s"
                      % (j["key"], len(bad), bad[0]), file=sys.stderr)
        ok += 1
    print("translate: %d/%d shader(s) -> %s" % (ok, len(jobs), a.out))
    print("next: xcrun -sdk iphoneos metal -c ... (macOS only, see docs/SHADER_CHAIN.md)")


def cmd_check(a):
    rc = 0
    try:
        r = subprocess.run(tool_cmd(a.xlate) + ["--source-sha"], capture_output=True,
                           text=True, timeout=60)
        sha = (r.stdout or "").strip()
        if r.returncode or not sha:
            print("[RED] dmsh_xlate --source-sha failed: %s"
                  % ((r.stderr or r.stdout or "").strip()[:300] or "no output"))
            rc = 1
        else:
            print("dmsh_xlate: source-sha %s" % sha)
    except Exception as e:
        print("[RED] dmsh_xlate: %s" % e)
        rc = 1
    try:
        v = subprocess.run([a.spirv_cross, "--version"], capture_output=True, text=True, timeout=60)
        out = (v.stdout or "") + (v.stderr or "")
        line = out.strip().splitlines()[0] if out.strip() else "?"
        print("spirv-cross: %s" % line)
        if "vulkan-sdk-1.4.363.0" not in line:
            print("[warn] not the pinned SPIRV-Cross release (vulkan-sdk-1.4.363.0); "
                  "the output may differ from the reference chain")
    except Exception as e:
        print("[RED] spirv-cross: %s" % e)
        rc = 1
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract", help="game package -> code files + manifest.tsv")
    e.add_argument("inputs", nargs="+", help=".pack/.sbfsha/.ssarc files or a directory")
    e.add_argument("--out", required=True)
    e.add_argument("--keep-full-code", action="store_true",
                   help="also write <key>_<stage>.code (full code area, 0x30 head "
                        "included) so scripts/shader_chain/keycheck.cpp can replay the "
                        "runtime's hash over real shaders")
    t = sub.add_parser("translate", help="manifest -> SPIR-V -> MSL")
    t.add_argument("--work", required=True, help="the --out directory of extract")
    t.add_argument("--out", required=True)
    t.add_argument("--xlate", required=True, help="dmsh_xlate(.exe|.dll)")
    t.add_argument("--spirv-cross", dest="spirv_cross", required=True)
    t.add_argument("--keys", nargs="*", help="only these 16-hex shader keys")
    t.add_argument("--limit", type=int, default=0)
    c = sub.add_parser("check-tools", help="print the chain tool versions")
    c.add_argument("--xlate", required=True)
    c.add_argument("--spirv-cross", dest="spirv_cross", required=True)
    a = ap.parse_args()
    if a.cmd == "extract":
        cmd_extract(a)
    elif a.cmd == "translate":
        cmd_translate(a)
    else:
        return cmd_check(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
