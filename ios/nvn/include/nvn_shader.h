/* The one seam between "the guest has shaders" and "Metal can use them".
 *
 * The guest hands NVN Maxwell SASS (a code address plus a control section).
 * Metal wants MSL.  Nothing in this repository translates between the two
 * yet, so nvngfx_shader_to_msl() returns NULL and says why.
 *
 * Everything downstream of this call -- pipeline creation, render passes,
 * draw encoding, readback -- is implemented and executed by the self-test
 * below against a built-in MSL program, so the translator is the only piece
 * missing rather than "the translator plus everything that consumes it".
 */
#ifndef NVN_SHADER_H
#define NVN_SHADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage order matches the index NVN uses in nvnProgramSetShaders. */
typedef enum {
    NVNGFX_STAGE_VERTEX = 0,
    NVNGFX_STAGE_FRAGMENT = 1,
    NVNGFX_STAGE_COMPUTE = 2
} nvngfx_stage;

/* Translates one guest shader to MSL source.  Returns NULL when it cannot;
 * `why` (may be NULL) receives a static, human-readable reason.
 *
 * `code` is the NVNbufferAddress the guest passed in NVNshaderData::data, i.e.
 * a guest GPU address, not a host pointer.  `size` is the code length when the
 * caller knows it, 0 otherwise (the length lives at control+0x704, which this
 * layer does not read yet).
 *
 * Today this always fails with "no translator".  That is the honest state:
 * see docs/IOS_PORTING.md, section 8. */
const char* nvngfx_shader_to_msl(const void* code, unsigned long long size,
                                 nvngfx_stage stage, const char** why);


/* ---------------------------------------------------------------------------
 * Offline MSL blobs
 *
 * The translator that exists in this project reads Maxwell SASS on a computer,
 * not on the phone: see docs/SHADER_CHAIN.md.  Its output is a set of MSL
 * sources named after the shader key, and this table is how they reach the
 * backend.
 *
 * The key is the one both this repository and nxnative use:
 *     sha256(first 0x880 bytes of the control section (its own length when
 *            shorter) || code area)[0..7]
 * printed as 16 lowercase hex digits.  The code area here is the one the guest
 * passes in NVNshaderData::data, *including* its 0x30 header.
 *
 * Lookup never copies or allocates: the table must stay alive for as long as
 * it is installed, which is why the generated one is a static array. */
/* ---------------------------------------------------------------------------
 * The argument layout
 *
 * spirv-cross puts every resource of a stage into one MSL argument buffer
 * (`struct spvDescriptorSetBuffer0`), and each member carries [[id(k)]].  The
 * member's *name* says which NVN binding it came from:
 *
 *     vp_c<N> / fp_c<N>          constant bank N -> uniform buffer slot N - 3
 *     vp_c1 / fp_c1              the embedded c1 segment (blob->c1)
 *     fp_t_tcb_<handle>          combined texture (the c2 handle table)
 *     fp_t_tcb_<handle>Smplr     the sampler carried by that combined handle
 *     fp_t_cb<N>_<handle>        a texture handle stored in uniform buffer N
 *     fp_t_cb<N>_<handle>Smplr   the sampler half of that bindless handle
 *     fp_i_... / fp_s<N>         image / storage buffer
 *
 * The generator (scripts/shader_chain/gen_blobs.py) decodes that naming into
 * the table below and ships it next to the MSL, so the backend never has to
 * guess an [[id(k)]] ordering at run time.  Anything the replay layer cannot
 * fill yet -- the c0/c2 driver tables, images, typed buffers, a type-3
 * constant sampler -- is recorded as NVNGFX_ARG_UNSUPPORTED, and a draw that
 * needs one is dropped instead of reading an unset word.
 *
 * Two real Breath of the Wild shaders measured on 2026-10-30: the vertex one
 * has 7 members (highest id 6), the fragment one 42 (highest id 41, of which
 * 5 are uniform buffers, 18 combined textures, 18 samplers and 1 the c1
 * bank).  That is why the bound is 64 and not 8. */
#define NVNGFX_MAX_ARGS 64
#define NVNGFX_MAX_ARG_ID (NVNGFX_MAX_ARGS - 1)

/* Slot ranges the NVN entry points accept, and the sizes the backend has to
 * allocate.  A combined texture slot r lives at c2 0x20 + 8r and the driver
 * table has 32 of them (0x20..0x120). */
#define NVNGFX_MAX_UBO_SLOTS 16
#define NVNGFX_MAX_TEXTURE_SLOTS 32
#define NVNGFX_MAX_SAMPLER_SLOTS 32

/* c1 is the shader's embedded constant segment (control +0x700/+0x704).
 * nxnative's measured BOTW library uses 0, 256, 512 or 768 bytes; 64 KiB is
 * the same hard ceiling its c1 banks use, so a corrupt sidecar cannot make the
 * phone allocate an unbounded buffer. */
#define NVNGFX_MAX_C1_BYTES (64u * 1024u)

typedef enum nvngfx_arg_kind {
    NVNGFX_ARG_UBO = 1,                /* slot = uniform buffer index r (c[3+r]) */
    NVNGFX_ARG_TEXTURE = 2,            /* slot = combined texture slot r */
    NVNGFX_ARG_SAMPLER_OF_TEXTURE = 3, /* slot = combined texture slot r; the
                                        * sampler comes from the same handle, so
                                        * there is no separate binding */
    NVNGFX_ARG_SEPARATE_TEXTURE = 4,   /* slot = separate texture slot r */
    NVNGFX_ARG_SEPARATE_SAMPLER = 5,   /* slot = separate sampler slot r */
    NVNGFX_ARG_C1 = 6,                 /* slot = -1; the data is blob->c1 */
    NVNGFX_ARG_UNSUPPORTED = 7,        /* see above: the draw cannot be filled */
    /* Bindless.  slot is the NVN uniform-buffer slot (c[3+slot]); offset is the
     * byte offset in that buffer.  The 32-bit value there is an NVN handle:
     * bits 0..19 name the texture and bits 20..31 the sampler. */
    NVNGFX_ARG_TEXTURE_IN_UBO = 8,     /* use the low 20 bits */
    NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE = 9, /* use the high 12 bits of the same word */
    NVNGFX_ARG_SAMPLER_IN_UBO = 10     /* type-1 handle: sampler word in another UBO */
} nvngfx_arg_kind;

typedef struct nvngfx_shader_arg {
    unsigned char id;    /* [[id(k)]] in the MSL argument buffer */
    unsigned char kind;  /* nvngfx_arg_kind */
    signed char slot;    /* NVN binding slot, or -1 for C1 / UNSUPPORTED */
    /* Member position in spvDescriptorSetBuffer0, zero based.  Metal packs an
     * argument buffer by declaration order, not by [[id(k)]], so a scalar
     * resource lives at byte 8*packed.  For N entries the packed values are a
     * permutation of 0..N-1.  This is why sparse ids (5/9/12) no longer leave
     * holes in the raw table. */
    unsigned char packed;
    /* Bindless only: byte offset of the 32-bit handle in uniform buffer
     * `slot`.  Zero for every other kind. */
    unsigned short offset;
} nvngfx_shader_arg;

typedef struct nvngfx_shader_blob {
    const char* key;   /* exactly 16 hex digits, any case */
    int stage;         /* nvngfx_stage; compute uses the direct-binding path */
    const char* msl;   /* NUL-terminated MSL source, entry point renamed below */
    /* Sorted by id, ids unique, 0..NVNGFX_MAX_ARGS entries.  NULL/0 when the
     * generator had no layout for this shader: the backend then falls back to
     * "uniform buffer i is id(i)" and drops any draw that binds a texture. */
    const nvngfx_shader_arg* args;
    unsigned num_args;
    /* Raw bytes of the embedded c1 segment, in guest order.  NULL/0 means the
     * sidecar had no c1 for this shader.  A non-NULL pointer with c1_size == 0
     * is an explicit empty c1 (some shaders declare it and read zeros); that
     * distinction must survive generation.  A blob with a C1 layout entry must
     * carry a non-NULL c1 pointer, even when its size is zero. */
    const unsigned char* c1;
    unsigned c1_size;
} nvngfx_shader_blob;

/* Installs a static table.  Nothing is copied.  Returns 0 on success, or
 * -(index + 1) naming the first bad entry (bad key, bad stage, empty MSL, or a
 * duplicate key+stage).  A layout is also checked: ids must be ascending and
 * unique, packed slots must be a permutation of 0..num_args-1, no id above
 * NVNGFX_MAX_ARG_ID, every kind in range with a slot that fits its table,
 * and C1 / UNSUPPORTED entries must carry slot -1.  A C1 entry must also name
 * a blob with c1 data (an empty blob is valid), c1_size
 * must not exceed NVNGFX_MAX_C1_BYTES, and a non-zero size needs a pointer.
 * On failure the previous table stays installed: the caller never sees a
 * half-installed table. */
int nvngfx_shader_set_blobs(const nvngfx_shader_blob* blobs, size_t count);

/* Uninstalls the table (lookups then fail with "no blob table"). */
void nvngfx_shader_clear_blobs(void);

/* Number of installed entries (0 with no table). */
size_t nvngfx_shader_blob_count(void);

/* Finds the MSL for `key` at `stage`.  Returns NULL and sets `why` when the
 * key is malformed, the table is empty, or the shader is not in it. */
const char* nvngfx_shader_lookup(const char* key, nvngfx_stage stage,
                                 const char** why);

/* The same lookup, returning the whole blob so the argument layout travels
 * with the MSL.  This is what the backend uses; nvngfx_shader_lookup() is the
 * MSL-only view. */
const nvngfx_shader_blob* nvngfx_shader_lookup_blob(const char* key,
                                                    nvngfx_stage stage,
                                                    const char** why);

/* ---------------------------------------------------------------------------
 * Offline MSL bundles
 *
 * The generated table is a C array; a title's few thousand shaders are better
 * shipped as files.  A bundle is a directory holding manifest.tsv plus one
 * .msl per stage and optional .c1 sidecars (format: ios/nvn/src/nvn_shader_bundle.c).
 *
 * Loading validates every entry first, then installs generated + bundle as one
 * table, so a build may ship both.  On failure nothing changes and `why` names
 * the first problem, including the manifest line number.
 *
 * On Apple platforms nvn_metal.mm looks in <app>/shaders; NVNGFX_SHADER_DIR
 * overrides that, which is how a freshly translated set is tried without
 * rebuilding the app.
 */
int nvngfx_shader_load_bundle(const char* dir, char* why, size_t why_size);

/* Entries the loaded bundle contributed (0 when none is installed). */
size_t nvngfx_shader_bundle_count(void);

/* Drops the bundle and reinstalls the generated table. */
void nvngfx_shader_unload_bundle(void);
/* The installed table and its length (NULL/0 when empty).  A caller that has
 * to swap the table out for a moment -- the Metal self-test does -- can put
 * the previous one back with nvngfx_shader_set_blobs(). */
const nvngfx_shader_blob* nvngfx_shader_blob_table(size_t* count);

/* Copies a blob's layout into `out`.  Returns the number of entries written,
 * or 0 when the blob has no layout or does not fit in `cap`.  Pure: this is
 * what tests pin and what the backend calls. */
unsigned nvngfx_shader_copy_args(const nvngfx_shader_blob* blob,
                                 nvngfx_shader_arg* out, unsigned cap);

/* The name the backend looks up in the library built from a blob.  Renaming is
 * done when the blob is generated (scripts/shader_chain/nx_shader.py), so the
 * two halves cannot drift. */
#define NVNGFX_ENTRY_VERTEX "nvngfx_main_vs"
#define NVNGFX_ENTRY_FRAGMENT "nvngfx_main_fs"
#define NVNGFX_ENTRY_COMPUTE "nvngfx_main_cs"


/* Computes a shader's key straight from the guest's control section and code
 * area (both are host-readable while nvnProgramSetShaders runs).  Writes 16
 * lowercase hex digits plus NUL; returns 0, or -1 when the control section
 * does not describe a code area.  The rule is the one written at the top of
 * ios/nvn/src/nvn_shader_key.c, the same one scripts/shader_chain uses. */
int nvngfx_shader_key_from_guest(const void* control, const void* code,
                                 char out[17]);

/* One-shot SHA-256.  Exposed so the portable test can pin it to the standard
 * vectors ("", "abc"), which is what makes the key above trustworthy. */
void nvngfx_sha256(const void* data, size_t len, unsigned char out[32]);

/* Installs the offline blob table.  Defined by ios/nvn/src/nvn_shader_blobs.gen.c,
 * which the committed tree ships empty; scripts/shader_chain/gen_blobs.py
 * rewrites it from a directory of translated .metal files. */
void nvngfx_shader_install_offline_blobs(void);

/* Host-only switch: when on, nvngfx_shader_to_msl() treats `code` as MSL
 * source (NUL-terminated within `size` when size is non-zero) and returns it
 * unchanged.  Nothing is translated; this is how the Metal half of the
 * backend gets executed before a translator exists.  Off by default and only
 * turned on by the self-test. */
void nvngfx_shader_set_msl_passthrough(int on);
int nvngfx_shader_msl_passthrough(void);

/* The program the self-test draws: a full-screen triangle generated from
 * [[vertex_id]] plus a constant colour.  Written for this repository; it is
 * not derived from, and does not contain, any title data. */
const char* nvngfx_shader_selftest_msl(nvngfx_stage stage);

/* Reads the compute threadgroup size from the machine-readable comment
 * `// nvngfx-threadgroup X Y Z` in a compute MSL blob.  Metal does not encode
 * the launch size in the kernel function, so NVN's group counts cannot be
 * replayed without this sidecar.  Returns 0 on success, -1 when the marker is
 * missing or the three dimensions are not positive / exceed 1024 total. */
int nvngfx_shader_threadgroup_size(const char* msl, unsigned out[3]);

/* Same, with a variant selector:
 *   0  the full-screen triangle built from [[vertex_id]] (no vertex buffer)
 *   1  the same triangle with positions from [[attribute(0)]] (float2) and a
 *      green fragment output -- this one needs a vertex descriptor, a stream
 *      stride and a vertex buffer, so the self-test can prove those work and
 *      the readback says which program drew.
 *   3  uv triangle + `[[id(0..2)]]` argument buffer (constant, texture,
 *      sampler), the layout spirv-cross emits for a translated stage
 *   4  the same draw with direct `[[texture(0)]]` / `[[sampler(0)]]`
 *      bindings: the non-Metal3 path, where gpuResourceID is zero
 *   5  variant 3 with the ids moved to 5 / 9 / 12, so the self-test can prove
 *      the table's stride and holes come from the layout sidecar
 *   6  a c1-only fragment shader
 *   7  two bindless resources (ids 9 / 12, packed 0 / 1) whose 32-bit handle
 *      lives in a uniform buffer */
const char* nvngfx_shader_selftest_msl_variant(nvngfx_stage stage, int variant);

#ifdef __cplusplus
}
#endif
#endif /* NVN_SHADER_H */