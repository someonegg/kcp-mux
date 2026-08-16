#include "kcpmux_hash.h"

#include <string.h>

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) && __GNUC__ >= 7
#define KCPMUX_FALL_THROUGH __attribute__((fallthrough))
#else
#define KCPMUX_FALL_THROUGH ((void)0)
#endif

static inline uint32_t
kcpmux_rotl32(uint32_t x, unsigned r)
{
    return (x << r) | (x >> (32u - r));
}

static inline uint32_t
kcpmux_load_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// MurmurHash3 x86_32, seed = 0
uint32_t kcpmux_hash32(const void *p, size_t len)
{
    const uint32_t c1 = UINT32_C(0xcc9e2d51);
    const uint32_t c2 = UINT32_C(0x1b873593);

    const uint8_t *data = (const uint8_t *)p;
    const size_t nblocks = len / 4;

    uint32_t hash = 0;

    // body
    for (size_t i = 0; i < nblocks; ++i) {
        uint32_t k = kcpmux_load_le32(data + i * 4);

        k *= c1;
        k = kcpmux_rotl32(k, 15);
        k *= c2;

        hash ^= k;
        hash = kcpmux_rotl32(hash, 13);
        hash = hash * UINT32_C(5) + UINT32_C(0xe6546b64);
    }

    // tail
    const size_t idx = nblocks * 4;
    uint32_t k1 = 0;

    switch (len - idx) {
    case 3:
        k1 ^= (uint32_t)data[idx + 2] << 16;
        KCPMUX_FALL_THROUGH;

    case 2:
        k1 ^= (uint32_t)data[idx + 1] << 8;
        KCPMUX_FALL_THROUGH;

    case 1:
        k1 ^= (uint32_t)data[idx];

        k1 *= c1;
        k1 = kcpmux_rotl32(k1, 15);
        k1 *= c2;

        hash ^= k1;
        break;

    default:
        break;
    }

    // finalization
    hash ^= (uint32_t)len;

    hash ^= hash >> 16;
    hash *= UINT32_C(0x85ebca6b);
    hash ^= hash >> 13;
    hash *= UINT32_C(0xc2b2ae35);
    hash ^= hash >> 16;

    return hash;
}

uint32_t kcpmux_hash32_str(const char *s)
{
    return kcpmux_hash32(s, strlen(s));
}
