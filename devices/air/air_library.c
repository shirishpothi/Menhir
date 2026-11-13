/* A metallib's function list (air_library.h). */

#include <string.h>

#include "air_library.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int air_library_functions(const uint8_t *lib, size_t len, AirFunction *out, int max) {
    if (len < 88 || memcmp(lib, "MTLB", 4)) return -1;
    uint64_t list = rd64(lib + 24), list_size = rd64(lib + 32), code = rd64(lib + 72), code_size = rd64(lib + 80);
    if (list + 4 > len || list_size > len - list || code > len || code_size > len - code) return -1;
    uint32_t count = rd32(lib + list);
    size_t at = list + 4, end = list + 4 + list_size;
    if (end > len) end = len;
    for (uint32_t i = 0; i < count; i++) {
        if (at + 4 > end) return -1;
        uint32_t size = rd32(lib + at);
        if (size < 4 || at + size > end) return -1;
        AirFunction f = {.type = -1};
        uint64_t mdsz = 0, offt = UINT64_MAX;
        for (size_t t = at + 4; t + 4 <= at + size;) {
            if (!memcmp(lib + t, "ENDT", 4)) break;
            if (t + 6 > at + size) return -1;
            uint16_t n = rd16(lib + t + 4);
            const uint8_t *d = lib + t + 6;
            if (t + 6 + n > at + size) return -1;
            if (!memcmp(lib + t, "NAME", 4)) {
                size_t k = n < sizeof f.name ? n : sizeof f.name - 1;
                memcpy(f.name, d, k);
                f.name[k] = 0;
            } else if (!memcmp(lib + t, "TYPE", 4) && n >= 1) f.type = d[0];
            else if (!memcmp(lib + t, "MDSZ", 4) && n >= 8) mdsz = rd64(d);
            else if (!memcmp(lib + t, "OFFT", 4) && n >= 24) offt = rd64(d + 16);
            t += 6 + n;
        }
        if (offt > code_size || mdsz > code_size - offt) return -1;
        f.bitcode = lib + code + offt, f.bitcode_size = (size_t)mdsz;
        /* The bitcode wrapper: {0x0B17C0DE, version, offset, size, cpu type}, then the bitcode ("BC\xc0\xde"). */
        if (f.bitcode_size >= 20 && rd32(f.bitcode) == 0x0B17C0DEu) {
            uint32_t off = rd32(f.bitcode + 8), n = rd32(f.bitcode + 12);
            if (off <= f.bitcode_size && n <= f.bitcode_size - off) f.bitcode += off, f.bitcode_size = n;
        }
        if ((int)i < max) out[i] = f;
        at += size;
    }
    return (int)count;
}
