#ifndef KCPMUX_TIMER_H
#define KCPMUX_TIMER_H

#include <stddef.h>
#include <stdint.h>

#include "kcpmux_types.h"
#include "kcpmux_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kcpmux_timer_node_s kcpmux_timer_node_t;
typedef void (*kcpmux_timer_cb)(kcpmux_timer_node_t *node, int64_t now_ms);

typedef enum kcpmux_timer_state_e {
    KCPMUX_TIMER_IDLE = 0,
    KCPMUX_TIMER_HEAP,
    KCPMUX_TIMER_DUE,
    KCPMUX_TIMER_RUNNING,
} kcpmux_timer_state_t;

struct kcpmux_timer_node_s {
    size_t                heap_index;
    int64_t               deadline_ms;
    uint64_t              insertion_sequence;
    void                 *owner;
    kcpmux_timer_cb       timeout_cb;
    list_head             due_link;
    kcpmux_timer_state_t  state;
};

typedef struct kcpmux_timer_manager_s {
    kcpmux_timer_node_t **heap;
    size_t               size;
    size_t               capacity;
    uint64_t             next_sequence;
} kcpmux_timer_manager_t;

static inline int64_t kcpmux_timer_deadline_after(int64_t anchor_ms,
                                                  uint32_t interval_ms) {
    if (anchor_ms > INT64_MAX - (int64_t)interval_ms) {
        return INT64_MAX;
    }
    return anchor_ms + (int64_t)interval_ms;
}

void kcpmux_timer_node_init(kcpmux_timer_node_t *node,
                           void *owner,
                           kcpmux_timer_cb timeout_cb);
int kcpmux_timer_manager_init(kcpmux_timer_manager_t *manager);
void kcpmux_timer_manager_destroy(kcpmux_timer_manager_t *manager);
int kcpmux_timer_manager_reserve(kcpmux_timer_manager_t *manager,
                                size_t capacity);
int kcpmux_timer_schedule(kcpmux_timer_manager_t *manager,
                         kcpmux_timer_node_t *node,
                         int64_t deadline_ms);
void kcpmux_timer_cancel(kcpmux_timer_manager_t *manager,
                        kcpmux_timer_node_t *node);
kcpmux_timer_node_t *kcpmux_timer_peek(kcpmux_timer_manager_t *manager);
void kcpmux_timer_collect_due(kcpmux_timer_manager_t *manager,
                             int64_t now_ms,
                             list_head *due_list);

#ifdef __cplusplus
}
#endif

#endif
