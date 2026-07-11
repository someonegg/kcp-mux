#ifndef __KCPMUX_LIST_H__
#define __KCPMUX_LIST_H__

#include <stddef.h>

#define kcpmux_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct list_head {
    struct list_head *next;
    struct list_head *prev;
} list_head;

#define list_entry(ptr, type, member) kcpmux_container_of(ptr, type, member)

static inline void list_init(list_head *list)
{
    list->next = list;
    list->prev = list;
}

#define INIT_LIST_HEAD(list) list_init(list)

static inline int list_empty(const list_head *head)
{
    return head->next == head;
}

static inline void __list_add(list_head *node, list_head *prev, list_head *next)
{
    next->prev = node;
    node->next = next;
    node->prev = prev;
    prev->next = node;
}

static inline void list_add_head(list_head *node, list_head *head)
{
    __list_add(node, head, head->next);
}

static inline void list_add_tail(list_head *node, list_head *head)
{
    __list_add(node, head->prev, head);
}

static inline void list_del(list_head *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry;
    entry->prev = entry;
}

static inline void list_move_tail(list_head *entry, list_head *head)
{
    list_del(entry);
    list_add_tail(entry, head);
}

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)

#define list_for_each_prev_safe(pos, tmp, head) \
    for ((pos) = (head)->prev, (tmp) = (pos)->prev; \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->prev)

#endif // __KCPMUX_LIST_H__
