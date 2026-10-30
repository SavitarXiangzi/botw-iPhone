#!/usr/bin/env python3
"""Turn translated MSL files into the C table the runtime looks keys up in.

Input: a directory of `<key>_<stage>.metal` files from nx_shader.py translate.
Output: ios/nvn/src/nvn_shader_blobs.gen.c (or --out), which defines
nvngfx_shader_install_offline_blobs().

Each blob also carries the stage's argument layout: spirv-cross emits one
`struct spvDescriptorSetBuffer0` whose members are `[[id(k)]]`, and the member
name encodes the NVN binding it came from (see nvn_shader.h).  Decoding that
here is what lets the backend fill `[[id(k)]]` without guessing:

    fp_c<N>                 uniform buffer slot N - 3
    fp_t_tcb_<h>            combined texture slot
    fp_t_tcb_<h>Smplr       the sampler carried by that same handle
    fp_t_cb<N>_<h> is bindless when 3 <= N <= 16, otherwise N == 2 names the
    driver handle table and decodes like any other texture
    fp_c1                    NVNGFX_ARG_C1, with the raw bytes from the
                             matching <key>_<stage>.c1 sidecar (empty is valid)
    fp_i_... / fp_s<N> / the c0-c2 driver banks: NVNGFX_ARG_UNSUPPORTED
    (a draw that needs one is dropped, not fed an unset word)

The committed copy of that file installs nothing; this is the only step that
puts title-derived text into a build, and it is meant to be run locally, not
committed.

    python scripts/shader_chain/gen_blobs.py --msl build/shader-chain/msl \
        --out ios/nvn/src/nvn_shader_blobs.gen.c
"""
import argparse
import os
import re
import sys

STAGES = {"vs": "NVNGFX_STAGE_VERTEX", "ps": "NVNGFX_STAGE_FRAGMENT"}
CHUNK = 4000
NAME = re.compile(r"^([0-9a-f]{16})_(vs|ps)\.metal$")

# Same bounds as nvn_shader.h; a table the runtime would refuse is a build
# error here, where the shader name is still known.
MAX_ARGS = 64
MAX_UBO_SLOTS = 16
MAX_TEXTURE_SLOTS = 32
MAX_SAMPLER_SLOTS = 32
MAX_C1_BYTES = 64 * 1024

TEX_BASE, IMG_BASE, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = 0x20, 0x120, 0x168, 128, 0x568

STRUCT = re.compile(r"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", re.S)
MEMBER = re.compile(r"^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];", re.M)


def decode_tex(name):
    """A spirv-cross resource name -> (texture slot, sampler slot, handle type).

    Ryujinx encodes a texture handle as three fields: the word offset in the
    constant bank (or c2), the second word's offset, and the handle type.  Type
    0 means "texture and sampler in one handle", so the sampler lives in the
    same slot as the texture; type 3 carries a constant sampler ID instead.

    This is a port of dmsh_build.py's decode_tex (nxnative, MIT); the numbers
    are the driver segment layout documented in docs/SHADER_CHAIN.md §6."""
    m = re.match(r"[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)", name)
    if not m:
        return None
    slots = int(m.group(1)) if m.group(1) else -1
    h = int(m.group(2), 16)
    tb = (slots & 0xFFFF) if slots >= 0 else 2
    sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3FFF, (h >> 14) & 0x3FFF, h >> 28

    def slot(word, base, cb):
        if cb != 2 or word * 4 < base or (word * 4 - base) % 8:
            return None
        return (word * 4 - base) // 8

    if re.match(r"[a-z]+_i", name):          # image: not this path
        return None, None, ty
    words = [o0] + ([o1] if ty else [])
    tw = [o0] if ty == 3 else words
    t = next((slot(x, TEX_BASE, tb) for x in tw
              if slot(x, TEX_BASE, tb) is not None and x * 4 < SEPTEX_BASE), None)
    if t is None:
        t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE, tb) for x in tw
                  if slot(x, SEPTEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words
              if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None) \
        if ty != 3 else None
    return t, s, ty


def bindless(name):
    """True for a handle that lives in the game's own uniform buffer.

    cb2 is the driver's texture handle table, so it decodes like any other
    combined texture; c3..c16 are the game's uniform buffers, where the replay
    layer would have to read the handle at run time."""
    m = re.match(r"[a-z]+_[ti]\w*?_cb(\d+)_", name)
    return bool(m) and 3 <= int(m.group(1)) <= 16


def classify(msl_type, name):
    """MSL struct member -> (C kind, slot).  slot is -1 when unsupported."""
    m = re.fullmatch(r"[a-z]+_c(\d+)", name)
    if m:
        n = int(m.group(1))
        if 3 <= n <= 16:
            return "NVNGFX_ARG_UBO", n - 3
        if n == 1:
            return "NVNGFX_ARG_C1", -1           # filled from blob->c1
        return "NVNGFX_ARG_UNSUPPORTED", -1      # c0, c2 driver tables
    if re.fullmatch(r"[a-z]+_s\d+", name):
        return "NVNGFX_ARG_UNSUPPORTED", -1      # storage buffer
    if re.match(r"[a-z]+_i", name):
        return "NVNGFX_ARG_UNSUPPORTED", -1      # image
    base = name[:-5] if name.endswith("Smplr") else name
    if bindless(base):
        return "NVNGFX_ARG_UNSUPPORTED", -1      # handle in a uniform buffer
    d = decode_tex(base)
    if d is None:
        return "NVNGFX_ARG_UNSUPPORTED", -1
    t, s, ty = d
    if name.endswith("Smplr"):
        if ty == 3:
            return "NVNGFX_ARG_UNSUPPORTED", -1  # constant sampler ID
        if ty == 0:
            # A separate texture's handle carries no sampler bits, so the
            # paired sampler cannot come from the same slot.
            if t is None or t >= SEPTEX_SLOT0:
                return "NVNGFX_ARG_UNSUPPORTED", -1
            return "NVNGFX_ARG_SAMPLER_OF_TEXTURE", t
        if ty == 1:
            return ("NVNGFX_ARG_SEPARATE_SAMPLER", s) if s is not None \
                else ("NVNGFX_ARG_UNSUPPORTED", -1)
        return "NVNGFX_ARG_UNSUPPORTED", -1
    if t is None:
        return "NVNGFX_ARG_UNSUPPORTED", -1
    if t >= SEPTEX_SLOT0:
        return "NVNGFX_ARG_SEPARATE_TEXTURE", t - SEPTEX_SLOT0
    return "NVNGFX_ARG_TEXTURE", t


def layout(text):
    """The stage's [[id(k)]] table as a list of (id, kind, slot), sorted by id.

    An empty list means the shader declares no resources (a constant-colour
    program), which is valid.  Raises ValueError for a table the runtime would
    refuse: an id twice or above MAX_ARGS, or a slot outside the API range."""
    body = STRUCT.search(text)
    if not body:
        return []
    out = []
    for m in MEMBER.finditer(body.group(1)):
        msl_type, name, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        if aid >= MAX_ARGS:
            raise ValueError("%s: [[id(%d)]] exceeds %d" % (name, aid, MAX_ARGS - 1))
        kind, slot = classify(msl_type, name)
        if kind == "NVNGFX_ARG_C1" and slot != -1:
            raise ValueError("%s: c1 slot must be -1" % name)
        if kind == "NVNGFX_ARG_UBO" and not 0 <= slot < MAX_UBO_SLOTS:
            raise ValueError("%s: uniform slot %d out of range" % (name, slot))
        if kind in ("NVNGFX_ARG_TEXTURE", "NVNGFX_ARG_SAMPLER_OF_TEXTURE",
                    "NVNGFX_ARG_SEPARATE_TEXTURE") and not 0 <= slot < MAX_TEXTURE_SLOTS:
            raise ValueError("%s: texture slot %d out of range" % (name, slot))
        if kind == "NVNGFX_ARG_SEPARATE_SAMPLER" and not 0 <= slot < MAX_SAMPLER_SLOTS:
            raise ValueError("%s: sampler slot %d out of range" % (name, slot))
        out.append((aid, kind, slot))
    out.sort()
    for a, b in zip(out, out[1:]):
        if a[0] == b[0]:
            raise ValueError("[[id(%d)]] appears twice" % a[0])
    return out


BS = chr(92)   # backslash, spelled out so this file needs no escape games
LF = chr(10)


def c_string(text):
    """Escape MSL for a C literal, split so no single literal is huge.

    Split the raw text and escape each chunk on its own: splitting the
    escaped text can cut an escape sequence in half and end the literal
    early (it did, and the C compiler caught it)."""
    text = text.replace(chr(13), "")
    parts = [text[i:i + CHUNK] for i in range(0, len(text), CHUNK)] or [""]
    out = []
    for p in parts:
        esc = p.replace(BS, BS + BS).replace(chr(34), BS + chr(34)).replace(LF, BS + "n")
        out.append(chr(34) + esc + chr(34))
    return (LF + "        ").join(out)

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--msl", required=True, help="directory of <key>_<vs|ps>.metal")
    ap.add_argument("--out", required=True, help="the .gen.c to write")
    a = ap.parse_args()

    rows = []
    errors = []
    for name in sorted(os.listdir(a.msl)):
        m = NAME.match(name)
        if not m:
            continue
        path = os.path.join(a.msl, name)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if not text.strip():
            print("[warn] %s is empty, skipped" % name, file=sys.stderr)
            continue
        try:
            args = layout(text)
        except ValueError as e:
            errors.append("%s: %s" % (name, e))
            continue
        c1_path = os.path.join(a.msl, "%s_%s.c1" % (m.group(1), m.group(2)))
        c1 = None
        c1_present = os.path.exists(c1_path)
        if c1_present:
            with open(c1_path, "rb") as f:
                c1 = f.read()
            if len(c1) > MAX_C1_BYTES:
                errors.append("%s: c1 is %d bytes, maximum is %d"
                              % (name, len(c1), MAX_C1_BYTES))
                continue
        if any(kind == "NVNGFX_ARG_C1" for _aid, kind, _slot in args) and not c1_present:
            errors.append("%s: MSL declares c1 but %s is missing"
                          % (name, os.path.basename(c1_path)))
            continue
        rows.append((m.group(1), m.group(2), text, args, c1, c1_present))
    if errors:
        for e in errors:
            print("[RED] %s" % e, file=sys.stderr)
        sys.exit("[RED] %d shader(s) have a layout the runtime would refuse"
                 % len(errors))
    if not rows:
        sys.exit("[RED] no <key>_<vs|ps>.metal in %s" % a.msl)

    # A key must have exactly one blob per stage; a duplicate is a build bug.
    seen = set()
    for key, st, _t, _a, _c1, _cp in rows:
        if (key, st) in seen:
            sys.exit("[RED] duplicate blob %s_%s" % (key, st))
        seen.add((key, st))

    out = ["/* GENERATED by scripts/shader_chain/gen_blobs.py -- do not edit.",
           " * %d shader(s): %d vs, %d ps.  Title-derived; do not commit. */"
           % (len(rows), sum(1 for r in rows if r[1] == "vs"),
              sum(1 for r in rows if r[1] == "ps")),
           '#include "nvn_shader.h"', ""]

    empty_c1 = any(cp and c1 == b"" for _k, _s, _t, _a, c1, cp in rows)
    if empty_c1:
        out.append("static const unsigned char kEmptyC1[] = {0};")
        out.append("")
    for key, st, _text, _args, c1, c1_present in rows:
        if not c1_present or not c1:
            continue
        out.append("static const unsigned char kC1_%s_%s[] = {" % (key, st))
        for i in range(0, len(c1), 16):
            out.append("    " + ", ".join("0x%02x" % b for b in c1[i:i + 16]) + ",")
        out.append("};")
        out.append("")

    for key, st, _text, args, _c1, _cp in rows:
        if not args:
            continue
        out.append("static const nvngfx_shader_arg kArgs_%s_%s[] = {" % (key, st))
        for aid, kind, slot in args:
            out.append("    {%d, %s, %d}," % (aid, kind, slot))
        out.append("};")

    out.append("")
    out.append("static const nvngfx_shader_blob kBlobs[] = {")
    for key, st, text, args, c1, c1_present in rows:
        arg_ref = ("kArgs_%s_%s, %d" % (key, st, len(args))) if args else "0, 0"
        if c1_present:
            c1_ref = ("kEmptyC1, 0" if c1 == b""
                      else "kC1_%s_%s, %d" % (key, st, len(c1)))
        else:
            c1_ref = "0, 0"
        out.append('    {"%s", %s,' % (key, STAGES[st]))
        out.append("        " + c_string(text) + ", " + arg_ref + ", " + c1_ref + "},")
    out.append("};")
    out.append("")
    out.append("void nvngfx_shader_install_offline_blobs(void) {")
    out.append("    nvngfx_shader_set_blobs(kBlobs, sizeof kBlobs / sizeof kBlobs[0]);")
    out.append("}")
    out.append("")

    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    counts = {}
    c1_bytes = 0
    for _k, _s, _t, args, c1, cp in rows:
        for _aid, kind, _slot in args:
            counts[kind] = counts.get(kind, 0) + 1
        if cp:
            c1_bytes += len(c1 or b"")
    total = sum(counts.values())
    print("gen_blobs: %d shader(s) -> %s (%.1f KiB)"
          % (len(rows), a.out, os.path.getsize(a.out) / 1024.0))
    print("gen_blobs: %d argument(s): %s; c1=%d shader(s), %d bytes"
          % (total, ", ".join(
              "%s=%d" % (k.replace("NVNGFX_ARG_", "").lower(), v)
              for k, v in sorted(counts.items())),
             sum(1 for _k, _s, _t, _a, _c, cp in rows if cp), c1_bytes))
    if counts.get("NVNGFX_ARG_UNSUPPORTED"):
        print("[warn] %d argument(s) are unsupported; a draw whose shader names "
              "one will be dropped" % counts["NVNGFX_ARG_UNSUPPORTED"],
              file=sys.stderr)


if __name__ == "__main__":
    main()
