/* FNV-1a 64-bit — the render-hash used across the tsp-mc9m evidence trail.
 * Same algorithm as a2-egl-pbuffer-test (canonical 256x256 RGBA pbuffer
 * reference hash: ece220fb80ed4271). */
#ifndef PF_FNV1A64_H
#define PF_FNV1A64_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t fnv1a64(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

#endif
