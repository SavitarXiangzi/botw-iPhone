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

#ifdef __cplusplus
}
#endif
#endif /* NVN_SHADER_H */