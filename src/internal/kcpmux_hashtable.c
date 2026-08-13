#include "kcpmux_hashtable.h"

#include <assert.h>
#include <stdlib.h>

#define KCPMUX_MAX(a, b) ((a) > (b) ? (a) : (b))

kcpmux_htb_t *kcpmux_htb_new(int size, kcpmux_htbcb_cmp cb_cmp, kcpmux_htbcb_free cb_free)
{
    int i;
    size = KCPMUX_MAX(11, size);

    kcpmux_htb_t *ht = (kcpmux_htb_t *)calloc(1, sizeof(kcpmux_htb_t));
    if (!ht) {
        goto L_FAILED_1;
    }

    ht->hashtable = (list_head *)calloc(size, sizeof(list_head));
    if (!ht->hashtable) {
        goto L_FAILED_2;
    }

    for (i = 0; i < size; i++) {
        list_init(&ht->hashtable[i]);
    }

    ht->num     = 0;
    ht->size    = size;
    ht->cb_cmp  = cb_cmp;
    ht->cb_free = cb_free;
    return ht;
L_FAILED_2:
    free(ht);
    ht = NULL;
L_FAILED_1:
    return NULL;
}

list_head *kcpmux_htb_find(kcpmux_htb_t *ht, void *key, uint32_t hash)
{
    if(!ht || !key) {
        return NULL;
    }

    list_head *p = NULL;
    list_for_each(p, &ht->hashtable[hash % ht->size]) {
        if (ht->cb_cmp(key, p)) {
            return p;
        }
    }
    return NULL;
}

list_head *kcpmux_htb_find_lru(kcpmux_htb_t *ht, void *key, uint32_t hash)
{
    if(!ht || !key) {
        return NULL;
    }

    list_head *p = NULL, *t = NULL;
    list_for_each_prev_safe(p, t, &ht->hashtable[hash % ht->size]) {
        if (ht->cb_cmp(key, p)) {
            list_move_tail(p, &ht->hashtable[hash % ht->size]);
            return p;
        }
    }
    return NULL;
}

void kcpmux_htb_add(kcpmux_htb_t *ht, list_head *node, void *key, uint32_t hash)
{
    if (!ht || !node) {
        assert(!"KCPMUX_ERR_NIL_PTR");
    }

    if (!kcpmux_htb_find(ht, key, hash)) {
        list_add_head(node, &ht->hashtable[hash % ht->size]);
        ht->num ++;
    }
}

void kcpmux_htb_add_direct(kcpmux_htb_t *ht, list_head *node, void *key, uint32_t hash)
{
    list_add_head(node, &ht->hashtable[hash % ht->size]);
    ht->num ++;
}

void kcpmux_htb_del(kcpmux_htb_t *ht, list_head *node)
{
    if (!ht || !node) {
        assert(!"KCPMUX_ERR_NIL_PTR");
    }

    if (!list_empty(node)) {
        list_del(node);
        ht->num --;
    }

    if (ht->cb_free) {
        ht->cb_free(node);
        node = NULL;
    }
}

int kcpmux_htb_del_by_key(kcpmux_htb_t *ht, void *key, uint32_t hash)
{
    if (!ht || !key) {
        return -1;
    }

    list_head *node = kcpmux_htb_find(ht, key, hash);
    if (node) {
        kcpmux_htb_del(ht, node);
    }
    return 0;
}

int kcpmux_htb_num(kcpmux_htb_t *ht)
{
    if (!ht) {
        return 0;
    }
    return ht->num;
}

void kcpmux_htb_flush(kcpmux_htb_t *ht)
{
    if (!ht) {
        return;
    }

    int i;
    for (i = 0; i < ht->size; i++) {
        list_head *p = NULL, *t = NULL;
        list_for_each_safe(p, t, &ht->hashtable[i]) {
            list_del(p);
            if (ht->cb_free) {
                ht->cb_free(p);
                p = NULL;
            }
        }
    }
    ht->num = 0;
}

void kcpmux_htb_destroy(kcpmux_htb_t *ht)
{
    if (ht) {
        kcpmux_htb_flush(ht);
        if (ht->hashtable) {
            free(ht->hashtable);
            ht->hashtable = NULL;
        }
        free(ht);
    }
}
