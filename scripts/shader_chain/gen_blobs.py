#!/usr/bin/env python3
"""Turn translated MSL files into the C table the runtime looks keys up in.

Input: a directory of `<key>_<stage>.metal` files from nx_shader.py translate.
Output: either a C table (--out), which defines
nvngfx_shader_install_offline_blobs(), or a file bundle (--bundle-out) with
manifest.tsv, .msl and .c1 files that nvn_shader_load_bundle() reads at startup.
Both may be emitted in one run.

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

    # The replaceable form used by a translated iOS app:
    python scripts/shader_chain/gen_blobs.py --msl build/shader-chain/msl \
        --bundle-out build/shader-chain/shaders
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


def decode_ubo_tex(name):
    """A bindless handle stored in one of the game's uniform buffers.

    `fp_t_cb7_2A` means the NVN handle at c7 word 0x2A.  Returns
    (texture, sampler), each (uniform_slot, byte_offset), or None.  Handle
    type 0 packs texture and sampler in one 32-bit word; type 1 names a second
    word for the sampler; type 3 has a constant sampler ID instead.

    This is a port of dmsh_build.py's decode_ubo_tex (nxnative, MIT)."""
    m = re.match(r"[a-z]+_[ti]\w*?_cb(\d+)_([0-9A-F]+)", name)
    if not m:
        return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    tb = slots & 0xffff
    sb = ((slots >> 16) - 1) if slots >> 16 else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if not 3 <= tb <= 16 or ty not in (0, 1, 3) or (ty == 1 and not 3 <= sb <= 16):
        return None
    texture = (tb - 3, o0 * 4)
    if ty == 3:
        sampler = ("id", o1)
    elif ty == 1:
        sampler = (sb - 3, o1 * 4)
    else:
        sampler = texture
    return texture, sampler


def classify(msl_type, name):
    """MSL struct member -> (C kind, slot, UBO byte offset).

    slot is the NVN binding slot, or -1 when unsupported.  offset is nonzero
    only for a bindless handle stored in a uniform buffer."""
    m = re.fullmatch(r"[a-z]+_c(\d+)", name)
    if m:
        n = int(m.group(1))
        if 3 <= n <= 16:
            return "NVNGFX_ARG_UBO", n - 3, 0
        if n == 1:
            return "NVNGFX_ARG_C1", -1, 0       # filled from blob->c1
        return "NVNGFX_ARG_UNSUPPORTED", -1, 0  # c0, c2 driver tables
    if re.fullmatch(r"[a-z]+_s\d+", name):
        return "NVNGFX_ARG_UNSUPPORTED", -1, 0  # storage buffer
    if re.match(r"[a-z]+_i", name):
        return "NVNGFX_ARG_UNSUPPORTED", -1, 0  # image
    base = name[:-5] if name.endswith("Smplr") else name
    ub = decode_ubo_tex(base)
    if ub:
        if "texture_buffer" in msl_type:
            return "NVNGFX_ARG_UNSUPPORTED", -1, 0  # bindless typed buffer
        tex, samp = ub
        if name.endswith("Smplr"):
            if samp[0] == "id":
                return "NVNGFX_ARG_UNSUPPORTED", -1, 0  # constant sampler ID
            if samp == tex:
                return "NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE", tex[0], tex[1]
            return "NVNGFX_ARG_SAMPLER_IN_UBO", samp[0], samp[1]
        return "NVNGFX_ARG_TEXTURE_IN_UBO", tex[0], tex[1]
    d = decode_tex(base)
    if d is None:
        return "NVNGFX_ARG_UNSUPPORTED", -1, 0
    t, s, ty = d
    if name.endswith("Smplr"):
        if ty == 3:
            return "NVNGFX_ARG_UNSUPPORTED", -1, 0      # constant sampler ID
        if ty == 0:
            # A separate texture's handle carries no sampler bits, so the
            # paired sampler cannot come from the same slot.
            if t is None or t >= SEPTEX_SLOT0:
                return "NVNGFX_ARG_UNSUPPORTED", -1, 0
            return "NVNGFX_ARG_SAMPLER_OF_TEXTURE", t, 0
        if ty == 1:
            return (("NVNGFX_ARG_SEPARATE_SAMPLER", s, 0) if s is not None
                    else ("NVNGFX_ARG_UNSUPPORTED", -1, 0))
        return "NVNGFX_ARG_UNSUPPORTED", -1, 0
    if t is None:
        return "NVNGFX_ARG_UNSUPPORTED", -1, 0
    if t >= SEPTEX_SLOT0:
        return "NVNGFX_ARG_SEPARATE_TEXTURE", t - SEPTEX_SLOT0, 0
    return "NVNGFX_ARG_TEXTURE", t, 0


def layout(text):
    """The stage's argument table as (id, kind, slot, packed, offset), by id.

    packed is the member's declaration index, which is the byte offset Metal
    actually uses (byte 8*packed).  An empty list means the shader declares no
    resources (a constant-colour program), which is valid.  Raises ValueError
    for a table the runtime would refuse: an id twice or above MAX_ARGS, more
    than MAX_ARGS members, an array occupying several argument slots (not
    modelled yet), or a slot outside the API range."""
    body = STRUCT.search(text)
    if not body:
        return []
    out = []
    packed = 0
    for m in MEMBER.finditer(body.group(1)):
        msl_type, name, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        if packed >= MAX_ARGS:
            raise ValueError("%s: more than %d argument members" % (name, MAX_ARGS))
        if aid >= MAX_ARGS:
            raise ValueError("%s: [[id(%d)]] exceeds %d" % (name, aid, MAX_ARGS - 1))
        if re.match(r"array\s*<", msl_type):
            raise ValueError("%s: descriptor arrays need packed-slot widths, "
                             "which are not implemented yet" % name)
        kind, slot, offset = classify(msl_type, name)
        if kind == "NVNGFX_ARG_C1" and slot != -1:
            raise ValueError("%s: c1 slot must be -1" % name)
        if kind in ("NVNGFX_ARG_UBO", "NVNGFX_ARG_TEXTURE_IN_UBO",
                    "NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE",
                    "NVNGFX_ARG_SAMPLER_IN_UBO") and not 0 <= slot < MAX_UBO_SLOTS:
            raise ValueError("%s: uniform slot %d out of range" % (name, slot))
        if (kind in ("NVNGFX_ARG_TEXTURE", "NVNGFX_ARG_SAMPLER_OF_TEXTURE",
                     "NVNGFX_ARG_SEPARATE_TEXTURE")
                and not 0 <= slot < MAX_TEXTURE_SLOTS):
            raise ValueError("%s: texture slot %d out of range" % (name, slot))
        if kind == "NVNGFX_ARG_SEPARATE_SAMPLER" and not 0 <= slot < MAX_SAMPLER_SLOTS:
            raise ValueError("%s: sampler slot %d out of range" % (name, slot))
        if offset & 3 or offset > 0xffff:
            raise ValueError("%s: bindless handle offset %d is not a valid byte offset"
                             % (name, offset))
        out.append((aid, kind, slot, packed, offset))
        packed += 1
    out.sort(key=lambda e: e[0])
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

ARG_NUMBER = {
    "NVNGFX_ARG_UBO": 1,
    "NVNGFX_ARG_TEXTURE": 2,
    "NVNGFX_ARG_SAMPLER_OF_TEXTURE": 3,
    "NVNGFX_ARG_SEPARATE_TEXTURE": 4,
    "NVNGFX_ARG_SEPARATE_SAMPLER": 5,
    "NVNGFX_ARG_C1": 6,
    "NVNGFX_ARG_UNSUPPORTED": 7,
    "NVNGFX_ARG_TEXTURE_IN_UBO": 8,
    "NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE": 9,
    "NVNGFX_ARG_SAMPLER_IN_UBO": 10,
}


def write_c_table(path, rows):
    """Emit the static C table used by builds that embed their translation."""
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
        for aid, kind, slot, packed_slot, offset in args:
            out.append("    {%d, %s, %d, %d, %d}," %
                       (aid, kind, slot, packed_slot, offset))
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

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


def write_bundle(path, rows):
    """Emit manifest.tsv plus one .msl/.c1 pair per translated stage.

    The runtime loader treats this directory exactly like the C table, but the
    files can be replaced without rebuilding the app.  The manifest is written
    last through a temporary name so a killed generator cannot leave a
    half-described bundle visible.
    """
    os.makedirs(path, exist_ok=True)
    lines = [
        "# GENERATED by scripts/shader_chain/gen_blobs.py -- title-derived; do not commit.",
        "# key\tstage\tmsl\tc1\targs",
    ]
    for key, st, text, args, c1, c1_present in rows:
        msl_name = "%s_%s.msl" % (key, st)
        with open(os.path.join(path, msl_name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
            if text and not text.endswith("\n"):
                f.write("\n")
        c1_name = "-"
        if c1_present:
            c1_name = "%s_%s.c1" % (key, st)
            with open(os.path.join(path, c1_name), "wb") as f:
                f.write(c1 or b"")
        args_text = "-"
        if args:
            parts = []
            for aid, kind, slot, packed_slot, offset in args:
                if kind not in ARG_NUMBER:
                    raise ValueError("unknown argument kind %s" % kind)
                parts.append("%d:%d:%d:%d:%d" %
                             (aid, slot, packed_slot, ARG_NUMBER[kind], offset))
            args_text = ",".join(parts)
        lines.append("\t".join((key, st, msl_name, c1_name, args_text)))
    manifest = os.path.join(path, "manifest.tsv")
    temporary = manifest + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(temporary, manifest)
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--msl", required=True, help="directory of <key>_<vs|ps>.metal")
    ap.add_argument("--out", help="optional C table (.gen.c) to write")
    ap.add_argument("--bundle-out",
                    help="optional directory for manifest.tsv + .msl/.c1 files")
    a = ap.parse_args()
    if not a.out and not a.bundle_out:
        ap.error("at least one of --out or --bundle-out is required")

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
        if any(kind == "NVNGFX_ARG_C1" for _aid, kind, _slot, _packed, _off in args) and not c1_present:
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

    if a.out:
        write_c_table(a.out, rows)
    if a.bundle_out:
        write_bundle(a.bundle_out, rows)

    counts = {}
    c1_bytes = 0
    for _k, _s, _t, args, c1, cp in rows:
        for _aid, kind, _slot, _packed, _off in args:
            counts[kind] = counts.get(kind, 0) + 1
        if cp:
            c1_bytes += len(c1 or b"")
    total = sum(counts.values())
    print("gen_blobs: %d shader(s)" % len(rows))
    if a.out:
        print("gen_blobs: C table -> %s (%.1f KiB)"
              % (a.out, os.path.getsize(a.out) / 1024.0))
    if a.bundle_out:
        print("gen_blobs: bundle -> %s/manifest.tsv"
              % a.bundle_out)
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
