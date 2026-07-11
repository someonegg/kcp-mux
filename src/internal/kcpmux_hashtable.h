#ifndef __KCPMUX_HASHTABLE_H__
#define __KCPMUX_HASHTABLE_H__

#include <stdint.h>

#include "kcpmux_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int      (*kcpmux_htbcb_cmp)(void *k, list_head *p);
typedef void        (*kcpmux_htbcb_free)(list_head *p);

typedef struct kcpmux_htb_t {
    list_head             *hashtable;
    kcpmux_htbcb_cmp        cb_cmp;
    kcpmux_htbcb_free       cb_free;
    int                    num;
    int                    size;
} kcpmux_htb_t;

kcpmux_htb_t *  kcpmux_htb_new(int size, kcpmux_htbcb_cmp cb_cmp, kcpmux_htbcb_free cb_free);

void           kcpmux_htb_destroy(kcpmux_htb_t *ht);

list_head *    kcpmux_htb_find(kcpmux_htb_t *ht, void *key, uint32_t hash);

list_head *    kcpmux_htb_find_lru(kcpmux_htb_t *ht, void *key, uint32_t hash);

void           kcpmux_htb_add(kcpmux_htb_t *ht, list_head *p, void *key, uint32_t hash);

void           kcpmux_htb_add_direct(kcpmux_htb_t *ht, list_head *node, void *key, uint32_t hash);

void           kcpmux_htb_del(kcpmux_htb_t *ht, list_head *p);

int            kcpmux_htb_del_by_key(kcpmux_htb_t *ht, void *key, uint32_t hash);

int            kcpmux_htb_num(kcpmux_htb_t *ht);

void           kcpmux_htb_flush(kcpmux_htb_t *ht);

#ifdef __cplusplus
}
#endif
#endif // __KCPMUX_HASHTABLE_H__
