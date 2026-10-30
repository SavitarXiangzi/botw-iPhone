/* Shader key: the identity an offline-translated MSL blob is filed under.
 *
 *   key = sha256(control[0:nctl] || code[0:ncode])[0..8] as 16 hex digits
 *   ncode = u32 at control + 0x704        (length of the code area)
 *   own   = u32 at control + 0x20         (length of the control section)
 *   nctl  = 0x760 <= own < 0x880 ? own : 0x880
 *
 * The same rule produces the file names in scripts/shader_chain (Python:
 * shader_key()) and is what nxnative's host calls prog_key, so a blob built on
 * a computer is found here without any further agreement.
 *
 * Plain C, no Apple headers: compiled and tested on Windows/Linux too.
 */
#include "nvn_shader.h"

#include <stdint.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t used;
    uint64_t bits;
} Sha256;

static uint32_t ror32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    }
    for (i = 16; i < 64; ++i) {
        const uint32_t x = w[i - 15], y = w[i - 2];
        const uint32_t s0 = ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3);
        const uint32_t s1 = ror32(y, 17) ^ ror32(y, 19) ^ (y >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; ++i) {
        const uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + K[i] + w[i];
        const uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha_init(Sha256* s) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(s->h, iv, sizeof iv);
    s->used = 0;
    s->bits = 0;
}

static void sha_update(Sha256* s, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    s->bits += (uint64_t)len * 8u;
    while (len != 0) {
        const size_t take = (sizeof s->buf - s->used) < len ? (sizeof s->buf - s->used) : len;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == sizeof s->buf) {
            compress(s->h, s->buf);
            s->used = 0;
        }
    }
}

static void sha_final(Sha256* s, uint8_t out[32]) {
    uint8_t tail[72];
    const uint64_t bits = s->bits;
    size_t n = s->used;
    int i;
    memcpy(tail, s->buf, n);
    tail[n++] = 0x80u;
    if (n > 56) {
        memset(tail + n, 0, 64 - n);
        compress(s->h, tail);
        n = 0;
    }
    memset(tail + n, 0, 56 - n);
    for (i = 0; i < 8; ++i) tail[56 + i] = (uint8_t)(bits >> (56u - 8u * (unsigned)i));
    compress(s->h, tail);
    for (i = 0; i < 8; ++i) {
        out[4 * i + 0] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void nvngfx_sha256(const void* data, size_t len, unsigned char out[32]) {
    Sha256 s;
    sha_init(&s);
    if (len != 0) sha_update(&s, data, len);
    sha_final(&s, out);
}

int nvngfx_shader_key_from_guest(const void* control, const void* code, char out[17]) {
    static const char hex[] = "0123456789abcdef";
    const uint8_t* ctl = (const uint8_t*)control;
    const uint8_t* cd = (const uint8_t*)code;
    uint32_t ncode = 0, own = 0, nctl;
    uint8_t digest[32];
    Sha256 s;
    int i;

    if (!control || !code || !out) return -1;
    memcpy(&ncode, ctl + 0x704, sizeof ncode);
    memcpy(&own, ctl + 0x20, sizeof own);
    /* A code length outside this range is not a code area but whatever the
     * guest left in an uninitialised control section. */
    if (ncode == 0 || ncode >= (1u << 24)) return -1;
    nctl = (own >= 0x760u && own < 0x880u) ? own : 0x880u;

    sha_init(&s);
    sha_update(&s, ctl, nctl);
    sha_update(&s, cd, ncode);
    sha_final(&s, digest);
    for (i = 0; i < 8; ++i) {
        out[2 * i + 0] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 0x0f];
    }
    out[16] = '\0';
    return 0;
}
