#include "kcpmux_hash.h"

#include <string.h>

#if defined(__GNUC__) && __GNUC__ >= 7
#define KCPMUX_FALL_THROUGH __attribute__((fallthrough))
#else
#define KCPMUX_FALL_THROUGH ((void)0)
#endif

#define rotateleft(n, d) ((((unsigned)(n) << (unsigned)(d))) | ((unsigned)(n) >> (sizeof(int)*8 - (unsigned)(d))))

// murmur3
uint32_t kcpmux_hash32(const void* p, size_t len)
{
#define C1_32   0xcc9e2d51
#define C2_32   0x1b873593
#define R1_32   15
#define R2_32   13
#define M_32    5
#define N_32    0xe6546b64
    int hash = 0;
    const uint8_t* data = (const uint8_t*)p;
    size_t nblocks = len >> 2;

    // body
    size_t i;
    for (i = 0; i < nblocks; i++) {
        int k = ((int32_t*)data)[i];

        // mix functions
        k *= C1_32;
        k = rotateleft(k, R1_32);
        k *= C2_32;
        hash ^= k;
        hash = rotateleft(hash, R2_32) * M_32 + N_32;
    }

    // tail
    size_t idx = nblocks << 2;
    int k1 = 0;
    switch (len - idx) {
        case 3:
            k1 ^= data[idx + 2] << 16;
            KCPMUX_FALL_THROUGH;
        case 2:
            k1 ^= data[idx + 1] << 8;
            KCPMUX_FALL_THROUGH;
        case 1:
            k1 ^= data[idx];

            // mix functions
            k1 *= C1_32;
            k1 = rotateleft(k1, R1_32);
            k1 *= C2_32;
            hash ^= k1;
    }

    // finalization
    hash ^= len;
    hash ^= (((unsigned)hash) >> 16);
    hash *= 0x85ebca6b;
    hash ^= (((unsigned)hash) >> 13);
    hash *= 0xc2b2ae35;
    hash ^= (((unsigned)hash) >> 16);
    return (unsigned)hash;
}

uint32_t kcpmux_hash32_str(const char* s)
{
    return kcpmux_hash32(s, strlen(s));
}
