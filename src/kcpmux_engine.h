#ifndef __KCPMUX_ENGINE_H__
#define __KCPMUX_ENGINE_H__

#include "kcpmux_types.h"
#include "kcpmux_kcp.h"
#include "kcpmux_timer.h"
#include "kcpmux_hashtable.h"

typedef struct kcpmux_pending_release_s kcpmux_pending_release_t;
typedef void (*kcpmux_release_cb)(kcpmux_pending_release_t *item);

struct kcpmux_pending_release_s {
    list_head          node;
    kcpmux_release_cb   release_cb;
};

// ============================================================================
// Engine internal structure
// ============================================================================

struct kcpmux_engine_s {
    kcpmux_engine_config_t     config;          // Engine config
    kcpmux_engine_callbacks_t  callbacks;       // Engine callbacks
    void                      *user_data;       // User data

    kcpmux_kcp_ops_t          *kcp_ops;         // KCP operations

    // Default configurations for conn/stream
    kcpmux_conn_config_t       default_conn_config;      // Default conn config
    kcpmux_stream_config_t     default_stream_config;    // Default stream config

    // Connection management
    kcpmux_htb_t             *conn_map;        // Connection hash table (key: peer_addr)
    uint32_t                  conn_count;      // Connection count
    kcpmux_engine_stats_t     stats;           // Engine statistics

    // Deadline scheduler
    kcpmux_timer_manager_t    timer_manager;
    size_t                    timer_node_count;
    uint8_t                   external_timer_armed;
    int64_t                   external_timer_deadline_ms;
    uint64_t                  timer_dispatch_count;

    uint8_t                   destroying;

    // Streams with KCP operations waiting for an application batch boundary.
    list_head                 pending_batch_streams;

    // Defer physical destruction until the current internal operation ends.
    uint32_t                  operation_depth;
    list_head                 pending_release_list;
};

// ============================================================================
// Engine internal functions
// ============================================================================

// Get current time (ms)
int64_t kcpmux_engine_now(kcpmux_engine_t *engine);

// Timer node lifecycle and scheduling
int kcpmux_engine_register_timer_node(
    kcpmux_engine_t *engine,
    kcpmux_timer_node_t *node,
    void *owner,
    kcpmux_timer_cb timeout_cb);
void kcpmux_engine_unregister_timer_node(kcpmux_engine_t *engine, kcpmux_timer_node_t *node);
int kcpmux_engine_schedule_timer_node(
    kcpmux_engine_t *engine,
    kcpmux_timer_node_t *node,
    int64_t deadline_ms,
    int64_t now_ms);
void kcpmux_engine_cancel_timer_node(
    kcpmux_engine_t *engine,
    kcpmux_timer_node_t *node,
    int64_t now_ms);
void kcpmux_engine_rearm_timer(kcpmux_engine_t *engine, int64_t now_ms);

// Keep terminal owners alive until the current internal operation returns.
void kcpmux_engine_operation_enter(kcpmux_engine_t *engine);
void kcpmux_engine_operation_leave(kcpmux_engine_t *engine);
void kcpmux_engine_queue_release(
    kcpmux_engine_t *engine,
    kcpmux_pending_release_t *item,
    kcpmux_release_cb release_cb);

// Send data to socket
// Return: 0 on success, < 0 on error
int kcpmux_engine_write_socket(
    kcpmux_engine_t *engine,
    const uint8_t *buf,
    unsigned size,
    const kcpmux_addr_t *addr);

// Add/remove connection
void kcpmux_engine_add_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn);
void kcpmux_engine_remove_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn);

#endif // __KCPMUX_ENGINE_H__
