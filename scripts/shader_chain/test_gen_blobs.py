#!/usr/bin/env python3
"""Test gen_blobs.py's argument-layout decoding on synthetic MSL.

The struct below has the shape spirv-cross emits, including the resource
naming (fp_c3, fp_t_tcb_1C, ...).  A real title's shader must not be copied
into this repository, so the names are written out by hand here and only the
*decoder* is exercised.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_blobs.py")
sys.path.insert(0, HERE)
import gen_blobs  # noqa: E402  (the decoder functions are under test too)

PS = """#include <metal_stdlib>
using namespace metal;

struct spvDescriptorSetBuffer0
{
    constant _16* fp_c3 [[id(0)]];
    constant _16* fp_c1 [[id(1)]];
    texture2d<float> fp_t_tcb_1C [[id(2)]];
    sampler fp_t_tcb_1CSmplr [[id(3)]];
    texture2d<float> fp_t_tcb_5A [[id(4)]];
    sampler fp_t_tcb_5ASmplr [[id(5)]];
    texture2d<float> fp_t_cb7_2A [[id(6)]];
    sampler fp_t_cb7_2ASmplr [[id(7)]];
    texture2d<float> fp_i_tcb_8 [[id(8)]];
    constant _16* fp_s3 [[id(9)]];
    constant _10* support_buffer [[id(10)]];
    constant _16* fp_c2 [[id(11)]];
};

fragment float4 nvngfx_main_fs(constant spvDescriptorSetBuffer0& s0 [[buffer(0)]])
{
    return s0.fp_t_tcb_1C.sample(s0.fp_t_tcb_1CSmplr, float2(0.0));
}
"""

VS = """#include <metal_stdlib>
using namespace metal;

vertex float4 nvngfx_main_vs(uint vid [[vertex_id]])
{
    return float4(0.0);
}
"""

BAD = PS.replace("[[id(9)]]", "[[id(64)]]")

EXPECTED_PS = [
    "{0, NVNGFX_ARG_UBO, 0, 0, 0},",
    "{1, NVNGFX_ARG_C1, -1, 1, 0},",             # embedded constant segment
    "{2, NVNGFX_ARG_TEXTURE, 10, 2, 0},",        # tcb 0x1C -> c2 0x70 -> slot 10
    "{3, NVNGFX_ARG_SAMPLER_OF_TEXTURE, 10, 3, 0},",
    "{4, NVNGFX_ARG_SEPARATE_TEXTURE, 0, 4, 0},", # tcb 0x5A -> c2 0x168 -> separate 0
    "{5, NVNGFX_ARG_UNSUPPORTED, -1, 5, 0},",     # that handle carries no sampler
    "{6, NVNGFX_ARG_TEXTURE_IN_UBO, 4, 6, 168},", # c7 word 0x2A -> +0xA8
    "{7, NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE, 4, 7, 168},",
    "{8, NVNGFX_ARG_UNSUPPORTED, -1, 8, 0},",     # image
    "{9, NVNGFX_ARG_UNSUPPORTED, -1, 9, 0},",     # storage buffer
    "{10, NVNGFX_ARG_SUPPORT, -1, 10, 0},",       # Ryujinx support buffer
    "{11, NVNGFX_ARG_DRIVER_C2, -1, 11, 0},",     # c2 handle tables
]

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print("FAIL " + what)
        failures += 1


def run(msl, out=None, bundle_out=None):
    argv = [sys.executable, GEN, "--msl", msl]
    if out:
        argv += ["--out", out]
    if bundle_out:
        argv += ["--bundle-out", bundle_out]
    return subprocess.run(argv, capture_output=True, text=True)


def block(text, name):
    m = re.search(r"static const nvngfx_shader_arg %s\[\] = \{(.*?)\};" % re.escape(name),
                  text, re.S)
    return None if not m else [ln.strip() for ln in m.group(1).splitlines() if ln.strip()]


def main():
    # Decoder-level cases that need no generated file.  Type 0 is what the
    # whole measured BOTW shader library uses; type 1 and 3 are included so the
    # generator is explicit rather than silently treating every bindless name
    # as type 0.
    tb, hs = gen_blobs.decode_ubo_tex("fp_t_cb7_2A")
    check(tb == (4, 168) and hs == (4, 168), "type-0 bindless decode")
    # cb851977 = 9 | (13 << 16): texture in UBO 6, sampler type-1 word in UBO 9.
    h1 = (1 << 28) | (0x2C << 14) | 0x2B
    tb, hs = gen_blobs.decode_ubo_tex("fp_t_cb851977_%X" % h1)
    check(tb == (6, 172) and hs == (9, 176), "type-1 bindless decode: %r %r" % (tb, hs))
    h3 = (3 << 28) | (0x104 << 14) | 0x28
    tb, hs = gen_blobs.decode_ubo_tex("fp_t_cb5_%X" % h3)
    check(tb == (2, 160) and hs == ("id", 0x104), "type-3 bindless decode: %r %r" % (tb, hs))
    check(gen_blobs.decode_ubo_tex("fp_t_cb2_1C") is None,
          "c2 is the driver table, not a bindless UBO handle")
    check(gen_blobs.classify("constant _10*", "support_buffer") ==
          ("NVNGFX_ARG_SUPPORT", -1, 0), "support buffer is recognised")
    check(gen_blobs.classify("constant _16*", "fp_c2") ==
          ("NVNGFX_ARG_DRIVER_C2", -1, 0), "c2 driver table is recognised")
    check(gen_blobs.classify("constant _16*", "fp_c0") ==
          ("NVNGFX_ARG_UNSUPPORTED", -1, 0), "c0 stays unsupported")
    check(gen_blobs.classify("sampler", "fp_t_cb5_%XSmplr" % h3) ==
          ("NVNGFX_ARG_UNSUPPORTED", -1, 0), "type-3 constant sampler is refused")
    # IDs may differ from declaration order; packed remains the declaration
    # index because that is the byte offset Metal uses.
    reordered = """struct spvDescriptorSetBuffer0 {
    texture2d<float> a [[id(9)]];
    sampler b [[id(2)]];
    texture2d<float> c [[id(5)]];
};
"""
    got = gen_blobs.layout(reordered)
    check([row[0] for row in got] == [2, 5, 9], "layout sorted by id")
    check([row[3] for row in got] == [1, 2, 0], "packed keeps declaration order")
    array_msl = """struct spvDescriptorSetBuffer0 {
    array<texture2d<float>, 4> tex [[id(0)]];
};
"""
    try:
        gen_blobs.layout(array_msl)
    except ValueError as e:
        check("array" in str(e).lower(), "array failure should explain itself: " + str(e))
    else:
        check(False, "descriptor array should be refused until packed widths exist")

    with tempfile.TemporaryDirectory() as tmp:
        msl = os.path.join(tmp, "msl")
        os.makedirs(msl)
        for name, text in (("aaaaaaaaaaaaaaaa_ps.metal", PS),
                           ("bbbbbbbbbbbbbbbb_vs.metal", VS),
                           ("cccccccccccccccc_ps.metal", BAD)):
            if text is BAD:
                continue
            with open(os.path.join(msl, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        c1_path = os.path.join(msl, "aaaaaaaaaaaaaaaa_ps.c1")
        c1 = bytes(range(16))
        with open(c1_path, "wb") as f:
            f.write(c1)
        out = os.path.join(tmp, "out.gen.c")
        r = run(msl, out)
        check(r.returncode == 0, "generator failed on a valid table: " + r.stderr.strip())
        with open(out, encoding="utf-8") as f:
            text = f.read()

        got = block(text, "kArgs_aaaaaaaaaaaaaaaa_ps")
        check(got == EXPECTED_PS, "ps layout is %r, want %r" % (got, EXPECTED_PS))
        # A vertex stage with no argument buffer gets no array, and its blob
        # row says "no layout" with two zeros.
        check("kArgs_bbbbbbbbbbbbbbbb_vs" not in text, "vs should have no layout array")
        check(re.search(r'"bbbbbbbbbbbbbbbb", NVNGFX_STAGE_VERTEX,\s*\n\s*".*?", 0, 0, 0, 0\},', text, re.S)
              is not None, "vs blob row should carry no layout and no c1")
        check("kC1_aaaaaaaaaaaaaaaa_ps" in text,
              "ps blob should carry the c1 byte array")
        check(re.search(r'"aaaaaaaaaaaaaaaa", NVNGFX_STAGE_FRAGMENT,\s*\n\s*".*?", '
                        r'kArgs_aaaaaaaaaaaaaaaa_ps, 12, kC1_aaaaaaaaaaaaaaaa_ps, 16\},',
                        text, re.S) is not None,
              "ps blob row should reference its layout and c1 bytes")
        check("unsupported=3" in r.stdout, "summary should count 3 unsupported: " + r.stdout)
        check("support=1" in r.stdout and "driver_c2=1" in r.stdout,
              "summary should count support and c2: " + r.stdout)
        check("texture_in_ubo=1" in r.stdout and "sampler_of_ubo_texture=1" in r.stdout,
              "summary should count both bindless halves: " + r.stdout)
        check("c1=1 shader(s), 16 bytes" in r.stdout,
              "summary should count c1 bytes: " + r.stdout)

        # Bundle output is the replaceable form the iOS app loads at startup.
        # It must describe exactly the same bytes and bindings as the C table.
        bundle = os.path.join(tmp, "bundle")
        r_bundle = run(msl, bundle_out=bundle)
        check(r_bundle.returncode == 0,
              "bundle generator failed: " + r_bundle.stderr.strip())
        manifest = os.path.join(bundle, "manifest.tsv")
        check(os.path.isfile(manifest), "bundle should write manifest.tsv")
        check(os.path.isfile(os.path.join(bundle, "aaaaaaaaaaaaaaaa_ps.msl")),
              "bundle should write the ps MSL")
        check(os.path.isfile(os.path.join(bundle, "bbbbbbbbbbbbbbbb_vs.msl")),
              "bundle should write the vs MSL")
        check(os.path.isfile(os.path.join(bundle, "aaaaaaaaaaaaaaaa_ps.c1")),
              "bundle should write the c1 sidecar")
        with open(os.path.join(bundle, "aaaaaaaaaaaaaaaa_ps.msl"),
                  encoding="utf-8") as f:
            check(f.read() == PS, "bundle MSL should be byte-for-byte source text")
        with open(os.path.join(bundle, "aaaaaaaaaaaaaaaa_ps.c1"), "rb") as f:
            check(f.read() == c1, "bundle c1 should be byte-for-byte source bytes")
        with open(manifest, encoding="utf-8") as f:
            manifest_text = f.read()
        entries = [ln for ln in manifest_text.splitlines()
                   if ln and not ln.startswith("#")]
        check(len(entries) == 2, "manifest should contain two entries: %r" % entries)
        want_args = ",".join(
            "%d:%d:%d:%d:%d" % (aid, slot, packed_slot,
                                 gen_blobs.ARG_NUMBER[kind], offset)
            for aid, kind, slot, packed_slot, offset in gen_blobs.layout(PS))
        want_ps = "\t".join(("aaaaaaaaaaaaaaaa", "ps", "aaaaaaaaaaaaaaaa_ps.msl",
                              "aaaaaaaaaaaaaaaa_ps.c1", want_args))
        check(want_ps in entries, "manifest ps row is wrong: %r" % entries)
        want_vs = "\t".join(("bbbbbbbbbbbbbbbb", "vs", "bbbbbbbbbbbbbbbb_vs.msl",
                              "-", "-"))
        check(want_vs in entries, "manifest vs row is wrong: %r" % entries)
        check("bundle -> %s/manifest.tsv" % bundle in r_bundle.stdout,
              "bundle summary should name its output: " + r_bundle.stdout)

        # At least one output is required; silently writing nothing is a bug.
        r_neither = subprocess.run([sys.executable, GEN, "--msl", msl],
                                   capture_output=True, text=True)
        check(r_neither.returncode != 0, "generator accepted no output option")
        check("--bundle-out" in (r_neither.stderr + r_neither.stdout),
              "no-output error should mention the options")

        # A c1 declaration without its sidecar is a build error, not a draw
        # that reads an unset pointer.
        os.remove(c1_path)
        missing = os.path.join(tmp, "missing.gen.c")
        r_missing = run(msl, missing)
        check(r_missing.returncode != 0, "generator accepted a missing c1 sidecar")
        check("c1" in (r_missing.stderr + r_missing.stdout).lower(),
              "missing c1 failure should name c1")
        check(not os.path.exists(missing), "no output on a missing c1 error")

        # A present but zero-length sidecar means an explicit empty c1.
        open(c1_path, "wb").close()
        empty = os.path.join(tmp, "empty.gen.c")
        r_empty = run(msl, empty)
        check(r_empty.returncode == 0, "generator rejected an empty c1: " + r_empty.stderr)
        with open(empty, encoding="utf-8") as f:
            empty_text = f.read()
        check("kEmptyC1" in empty_text, "empty c1 should use the shared zero array")
        check("kEmptyC1, 0" in empty_text, "empty c1 should have size zero")
        empty_bundle = os.path.join(tmp, "empty-bundle")
        r_empty_bundle = run(msl, bundle_out=empty_bundle)
        check(r_empty_bundle.returncode == 0,
              "bundle rejected an empty c1: " + r_empty_bundle.stderr)
        empty_c1 = os.path.join(empty_bundle, "aaaaaaaaaaaaaaaa_ps.c1")
        check(os.path.isfile(empty_c1), "empty c1 must remain an explicit file")
        check(os.path.getsize(empty_c1) == 0, "explicit c1 should be zero bytes")
        with open(os.path.join(empty_bundle, "manifest.tsv"), encoding="utf-8") as f:
            check("aaaaaaaaaaaaaaaa_ps.c1" in f.read(),
                  "manifest must not turn an empty c1 into '-'")

        # And an oversized sidecar is refused before it can allocate.
        with open(c1_path, "wb") as f:
            f.write(b"x" * (64 * 1024 + 1))
        large = os.path.join(tmp, "large.gen.c")
        r_large = run(msl, large)
        check(r_large.returncode != 0, "generator accepted an oversized c1")
        check(not os.path.exists(large), "no output on an oversized c1 error")
        with open(c1_path, "wb") as f:
            f.write(c1)

        # An id the runtime would refuse must stop the build, not ship silently.
        with open(os.path.join(msl, "cccccccccccccccc_ps.metal"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write(BAD)
        out2 = os.path.join(tmp, "bad.gen.c")
        r2 = run(msl, out2)
        check(r2.returncode != 0, "generator accepted [[id(64)]]")
        check("id(64)" in (r2.stderr + r2.stdout), "failure should name the id")
        check(not os.path.exists(out2), "no output may be written on a layout error")

    if failures:
        print("test_gen_blobs: %d failure(s)" % failures)
        return 1
    print("test_gen_blobs: ok (ids, packed slots, bindless, c1, bundle, holes, refusals)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())