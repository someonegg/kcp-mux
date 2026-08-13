#include "kcpmux_timer.h"

#include <stdlib.h>

#define KCPMUX_TIMER_INVALID_INDEX ((size_t)-1)

static int kcpmux_timer_less(const kcpmux_timer_node_t *left, const kcpmux_timer_node_t *right)
{
    if (left->deadline_ms != right->deadline_ms) {
        return left->deadline_ms < right->deadline_ms;
    }
    return left->insertion_sequence < right->insertion_sequence;
}

static void kcpmux_timer_set(
    kcpmux_timer_manager_t *manager,
    size_t index,
    kcpmux_timer_node_t *node)
{
    manager->heap[index] = node;
    node->heap_index = index;
}

static size_t kcpmux_timer_sift_up(kcpmux_timer_manager_t *manager, size_t index)
{
    kcpmux_timer_node_t *node = manager->heap[index];

    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!kcpmux_timer_less(node, manager->heap[parent])) {
            break;
        }
        kcpmux_timer_set(manager, index, manager->heap[parent]);
        index = parent;
    }
    kcpmux_timer_set(manager, index, node);
    return index;
}

static void kcpmux_timer_sift_down(kcpmux_timer_manager_t *manager, size_t index)
{
    kcpmux_timer_node_t *node = manager->heap[index];

    for (;;) {
        size_t left = index * 2 + 1;
        size_t right;
        size_t child;

        if (left >= manager->size) {
            break;
        }
        right = left + 1;
        child = left;
        if (right < manager->size &&
            kcpmux_timer_less(manager->heap[right], manager->heap[left])) {
            child = right;
        }
        if (!kcpmux_timer_less(manager->heap[child], node)) {
            break;
        }
        kcpmux_timer_set(manager, index, manager->heap[child]);
        index = child;
    }
    kcpmux_timer_set(manager, index, node);
}

static kcpmux_timer_node_t *kcpmux_timer_remove_at(kcpmux_timer_manager_t *manager, size_t index)
{
    kcpmux_timer_node_t *removed = manager->heap[index];
    kcpmux_timer_node_t *last = manager->heap[--manager->size];

    if (index < manager->size) {
        kcpmux_timer_set(manager, index, last);
        index = kcpmux_timer_sift_up(manager, index);
        kcpmux_timer_sift_down(manager, index);
    }
    manager->heap[manager->size] = NULL;
    removed->heap_index = KCPMUX_TIMER_INVALID_INDEX;
    return removed;
}

void kcpmux_timer_node_init(kcpmux_timer_node_t *node, void *owner, kcpmux_timer_cb timeout_cb)
{
    if (!node) {
        return;
    }
    node->heap_index = KCPMUX_TIMER_INVALID_INDEX;
    node->deadline_ms = 0;
    node->insertion_sequence = 0;
    node->owner = owner;
    node->timeout_cb = timeout_cb;
    INIT_LIST_HEAD(&node->due_link);
    node->state = KCPMUX_TIMER_IDLE;
}

int kcpmux_timer_manager_init(kcpmux_timer_manager_t *manager)
{
    if (!manager) {
        return KCPMUX_ERR_INVALID_PARAM;
    }
    manager->heap = NULL;
    manager->size = 0;
    manager->capacity = 0;
    manager->next_sequence = 0;
    return KCPMUX_ERR_OK;
}

void kcpmux_timer_manager_destroy(kcpmux_timer_manager_t *manager)
{
    if (!manager) {
        return;
    }
    free(manager->heap);
    manager->heap = NULL;
    manager->size = 0;
    manager->capacity = 0;
    manager->next_sequence = 0;
}

int kcpmux_timer_manager_reserve(kcpmux_timer_manager_t *manager, size_t capacity)
{
    kcpmux_timer_node_t **heap;
    size_t new_capacity;

    if (!manager) {
        return KCPMUX_ERR_INVALID_PARAM;
    }
    if (capacity <= manager->capacity) {
        return KCPMUX_ERR_OK;
    }
    new_capacity = manager->capacity ? manager->capacity : 8;
    while (new_capacity < capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = capacity;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(*heap)) {
        return KCPMUX_ERR_OOM;
    }
    heap = (kcpmux_timer_node_t **)realloc(manager->heap, new_capacity * sizeof(*heap));
    if (!heap) {
        return KCPMUX_ERR_OOM;
    }
    manager->heap = heap;
    manager->capacity = new_capacity;
    return KCPMUX_ERR_OK;
}

int kcpmux_timer_schedule(
    kcpmux_timer_manager_t *manager,
    kcpmux_timer_node_t *node,
    int64_t deadline_ms)
{
    size_t index;
    int ret;

    if (!manager || !node) {
        return KCPMUX_ERR_INVALID_PARAM;
    }
    if (node->state == KCPMUX_TIMER_HEAP) {
        node->deadline_ms = deadline_ms;
        index = kcpmux_timer_sift_up(manager, node->heap_index);
        kcpmux_timer_sift_down(manager, index);
        return KCPMUX_ERR_OK;
    }
    ret = kcpmux_timer_manager_reserve(manager, manager->size + 1);
    if (ret != KCPMUX_ERR_OK) {
        return ret;
    }
    if (node->state == KCPMUX_TIMER_DUE) {
        list_del_init(&node->due_link);
    }
    node->deadline_ms = deadline_ms;
    node->insertion_sequence = manager->next_sequence++;
    node->state = KCPMUX_TIMER_HEAP;
    index = manager->size++;
    kcpmux_timer_set(manager, index, node);
    kcpmux_timer_sift_up(manager, index);
    return KCPMUX_ERR_OK;
}

void kcpmux_timer_cancel(kcpmux_timer_manager_t *manager, kcpmux_timer_node_t *node)
{
    if (!manager || !node) {
        return;
    }
    if (node->state == KCPMUX_TIMER_HEAP) {
        kcpmux_timer_remove_at(manager, node->heap_index);
    } else if (node->state == KCPMUX_TIMER_DUE) {
        list_del_init(&node->due_link);
    }
    node->state = KCPMUX_TIMER_IDLE;
}

kcpmux_timer_node_t *kcpmux_timer_peek(kcpmux_timer_manager_t *manager)
{
    if (!manager || manager->size == 0) {
        return NULL;
    }
    return manager->heap[0];
}

void kcpmux_timer_collect_due(kcpmux_timer_manager_t *manager, int64_t now_ms, list_head *due_list)
{
    kcpmux_timer_node_t *node;

    if (!manager || !due_list) {
        return;
    }
    while ((node = kcpmux_timer_peek(manager)) != NULL) {
        if (node->deadline_ms > now_ms) {
            break;
        }
        node = kcpmux_timer_remove_at(manager, 0);
        node->state = KCPMUX_TIMER_DUE;
        list_add_tail(&node->due_link, due_list);
    }
}
