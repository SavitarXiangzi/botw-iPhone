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
typedef struct nvngfx_shader_blob {
    const char* key;   /* exactly 16 hex digits, any case */
    int stage;         /* nvngfx_stage; compute blobs are not accepted yet */
    const char* msl;   /* NUL-terminated MSL source, entry point renamed below */
} nvngfx_shader_blob;

/* Installs a static table.  Nothing is copied.  Returns 0 on success, or
 * -(index + 1) naming the first bad entry (bad key, bad stage, empty MSL, or a
 * duplicate key+stage).  On failure the previous table stays installed: the
 * caller never sees a half-installed table. */
int nvngfx_shader_set_blobs(const nvngfx_shader_blob* blobs, size_t count);

/* Uninstalls the table (lookups then fail with "no blob table"). */
void nvngfx_shader_clear_blobs(void);

/* Number of installed entries (0 with no table). */
size_t nvngfx_shader_blob_count(void);

/* Finds the MSL for `key` at `stage`.  Returns NULL and sets `why` when the
 * key is malformed, the table is empty, or the shader is not in it. */
const char* nvngfx_shader_lookup(const char* key, nvngfx_stage stage,
                                 const char** why);

/* The name the backend looks up in the library built from a blob.  Renaming is
 * done when the blob is generated (scripts/shader_chain/nx_shader.py), so the
 * two halves cannot drift. */
#define NVNGFX_ENTRY_VERTEX "nvngfx_main_vs"
#define NVNGFX_ENTRY_FRAGMENT "nvngfx_main_fs"


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

/* Same, with a variant selector:
 *   0  the full-screen triangle built from [[vertex_id]] (no vertex buffer)
 *   1  the same triangle with positions from [[attribute(0)]] (float2) and a
 *      green fragment output -- this one needs a vertex descriptor, a stream
 *      stride and a vertex buffer, so the self-test can prove those work and
 *      the readback says which program drew. */
const char* nvngfx_shader_selftest_msl_variant(nvngfx_stage stage, int variant);

#ifdef __cplusplus
}
#endif
#endif /* NVN_SHADER_H */