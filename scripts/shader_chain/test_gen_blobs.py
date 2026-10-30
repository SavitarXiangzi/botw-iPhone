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
    "{0, NVNGFX_ARG_UBO, 0},",
    "{1, NVNGFX_ARG_C1, -1},",             # embedded constant segment
    "{2, NVNGFX_ARG_TEXTURE, 10},",          # tcb handle 0x1C -> c2 0x70 -> slot 10
    "{3, NVNGFX_ARG_SAMPLER_OF_TEXTURE, 10},",
    "{4, NVNGFX_ARG_SEPARATE_TEXTURE, 0},",  # tcb handle 0x5A -> c2 0x168 -> separate 0
    "{5, NVNGFX_ARG_UNSUPPORTED, -1},",      # that handle carries no sampler
    "{6, NVNGFX_ARG_UNSUPPORTED, -1},",      # bindless: handle in uniform buffer 7
    "{7, NVNGFX_ARG_UNSUPPORTED, -1},",
    "{8, NVNGFX_ARG_UNSUPPORTED, -1},",      # image
    "{9, NVNGFX_ARG_UNSUPPORTED, -1},",      # storage buffer
]

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print("FAIL " + what)
        failures += 1


def run(msl, out):
    return subprocess.run([sys.executable, GEN, "--msl", msl, "--out", out],
                          capture_output=True, text=True)


def block(text, name):
    m = re.search(r"static const nvngfx_shader_arg %s\[\] = \{(.*?)\};" % re.escape(name),
                  text, re.S)
    return None if not m else [ln.strip() for ln in m.group(1).splitlines() if ln.strip()]


def main():
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
                        r'kArgs_aaaaaaaaaaaaaaaa_ps, 10, kC1_aaaaaaaaaaaaaaaa_ps, 16\},',
                        text, re.S) is not None,
              "ps blob row should reference its layout and c1 bytes")
        check("unsupported=5" in r.stdout, "summary should count 5 unsupported: " + r.stdout)
        check("c1=1 shader(s), 16 bytes" in r.stdout,
              "summary should count c1 bytes: " + r.stdout)

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
    print("test_gen_blobs: ok (ids, slots, c1, holes, refusals)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())