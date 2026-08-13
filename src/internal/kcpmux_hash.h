#ifndef __KCPMUX_HASH_H__
#define __KCPMUX_HASH_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t kcpmux_hash32(const void *p, size_t len);
uint32_t kcpmux_hash32_str(const char *s);

#ifdef __cplusplus
}
#endif
#endif // __KCPMUX_HASH_H__
