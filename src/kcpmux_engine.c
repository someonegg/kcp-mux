#include "kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_protocol.h"
#include "kcpmux_hash.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Hash table helper functions
// ============================================================================

// Connection compare function (variable length address data)
// Returns: 1 if match, 0 if not match
static int __conn_cmp(void *key, list_head *entry) {
    const kcpmux_addr_t *addr = (const kcpmux_addr_t *)key;
    kcpmux_conn_t *conn = list_entry(entry, kcpmux_conn_t, hash_node);
    if (addr->addrlen != conn->peer_addr.addrlen) {
        return 0;
    }
    return memcmp(addr->addr, conn->peer_addr.addr, addr->addrlen) == 0;
}

// Connection free function (called when hash table is destroyed)
static void __conn_free(list_head *entry) {
    // Note: don't free conn here, let engine handle it.
    (void)entry;
}

static void kcpmux_engine_drain_pending_release(kcpmux_engine_t *engine) {
    while (!list_empty(&engine->pending_release_list)) {
        kcpmux_pending_release_t *item = list_first_entry(
            &engine->pending_release_list, kcpmux_pending_release_t, node);
        list_del_init(&item->node);
        if (item->release_cb) {
            item->release_cb(item);
        }
    }
}

// ============================================================================
// Configuration init
// ============================================================================

void kcpmux_engine_config_init(kcpmux_engine_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
}

void kcpmux_conn_config_init(kcpmux_conn_config_t *config) {
    if (!config) return;
    config->ctrl_timeout_ms       = KCPMUX_DEFAULT_CONTROL_TIMEOUT_MS;
    config->connect_retries       = KCPMUX_DEFAULT_CONNECT_RETRIES;
    config->close_retries         = KCPMUX_DEFAULT_CLOSE_RETRIES;
    config->keepalive_interval_ms = KCPMUX_DEFAULT_KEEPALIVE_INTERVAL_MS;
    config->keepalive_timeout_ms  = KCPMUX_DEFAULT_KEEPALIVE_TIMEOUT_MS;
    config->idle_timeout_ms       = KCPMUX_DEFAULT_IDLE_TIMEOUT_MS;
}

void kcpmux_stream_config_init(kcpmux_stream_config_t *config) {
    if (!config) return;
    config->ctrl_timeout_ms       = KCPMUX_DEFAULT_SCONTROL_TIMEOUT_MS;
    config->close_retries         = KCPMUX_DEFAULT_SCLOSE_RETRIES;
    config->kcp_mss               = KCPMUX_DEFAULT_KCP_MSS;
    config->send_pause_threshold  = KCPMUX_DEFAULT_SEND_PAUSE_THRESHOLD;
    config->send_resume_threshold = KCPMUX_DEFAULT_SEND_RESUME_THRESHOLD;
}

// ============================================================================
// Engine lifecycle
// ============================================================================

kcpmux_engine_t *kcpmux_engine_create(
    const kcpmux_engine_config_t *config,
    const kcpmux_conn_config_t *default_conn_config,
    const kcpmux_stream_config_t *default_stream_config,
    const kcpmux_engine_callbacks_t *callbacks,
    void *user_data,
    const kcpmux_kcp_ops_t * kcp_ops)
{
    if (!callbacks) return NULL;

    kcpmux_engine_t *engine = (kcpmux_engine_t *)malloc(sizeof(kcpmux_engine_t));
    if (!engine) return NULL;

    memset(engine, 0, sizeof(*engine));
    INIT_LIST_HEAD(&engine->pending_release_list);
    if (kcpmux_timer_manager_init(&engine->timer_manager) != KCPMUX_ERR_OK) {
        free(engine);
        return NULL;
    }

    // Config
    if (config) {
        engine->config = *config;
    } else {
        kcpmux_engine_config_init(&engine->config);
    }

    // Default conn config
    if (default_conn_config) {
        engine->default_conn_config = *default_conn_config;
    } else {
        kcpmux_conn_config_init(&engine->default_conn_config);
    }

    // Default stream config
    if (default_stream_config) {
        engine->default_stream_config = *default_stream_config;
    } else {
        kcpmux_stream_config_init(&engine->default_stream_config);
    }

    // KCP
    if (kcp_ops) {
        engine->kcp_ops = (kcpmux_kcp_ops_t *)kcp_ops;
    } else {
        engine->kcp_ops = kcpmux_default_kcp_ops();
    }

    // Callbacks
    engine->callbacks = *callbacks;
    engine->user_data = user_data;

    // Create connection hash table
    engine->conn_map = kcpmux_htb_new(64, __conn_cmp, __conn_free);
    if (!engine->conn_map) {
        kcpmux_timer_manager_destroy(&engine->timer_manager);
        free(engine);
        return NULL;
    }

    return engine;
}

void kcpmux_engine_destroy(kcpmux_engine_t *engine) {
    if (!engine) return;

    engine->destroying = 1;
    kcpmux_engine_operation_enter(engine);
    // Finalize all connections through the normal ownership path.
    while (engine->conn_count > 0) {
        // Get first connection from hash table
        list_head *node = NULL;
        for (int i = 0; i < engine->conn_map->size && !node; i++) {
            if (!list_empty(&engine->conn_map->hashtable[i])) {
                node = engine->conn_map->hashtable[i].next;
            }
        }
        if (node) {
            kcpmux_conn_t *conn = list_entry(node, kcpmux_conn_t, hash_node);
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_NORMAL);
        } else {
            break;
        }
    }

    kcpmux_engine_operation_leave(engine);

    // All pending releases have drained at the operation boundary.
    kcpmux_htb_destroy(engine->conn_map);
    kcpmux_timer_manager_destroy(&engine->timer_manager);

    free(engine);
}

void kcpmux_engine_set_config(kcpmux_engine_t *engine,
                              const kcpmux_engine_config_t *config)
{
    if (!engine || !config) return;

    // Directly replace the entire config structure
    engine->config = *config;
}

// ============================================================================
// Engine main loop
// ============================================================================

void kcpmux_engine_update(kcpmux_engine_t *engine) {
    if (!engine) return;
    int64_t now = kcpmux_engine_now(engine);
    list_head due_list;

    // An update consumes the currently armed one-shot. This is also safe for
    // explicit early updates: the current root will simply be armed again.
    engine->external_timer_armed = 0;

    INIT_LIST_HEAD(&due_list);
    kcpmux_timer_collect_due(&engine->timer_manager, now, &due_list);

    kcpmux_engine_operation_enter(engine);
    while (!list_empty(&due_list)) {
        kcpmux_timer_node_t *node = list_first_entry(
            &due_list, kcpmux_timer_node_t, due_link);
        list_del_init(&node->due_link);
        node->state = KCPMUX_TIMER_RUNNING;
        engine->timer_dispatch_count++;
        if (node->timeout_cb) {
            node->timeout_cb(node, now);
        }
        if (node->state == KCPMUX_TIMER_RUNNING) {
            node->state = KCPMUX_TIMER_IDLE;
        }
    }
    kcpmux_engine_operation_leave(engine);
}

int kcpmux_engine_input(kcpmux_engine_t *engine,
                       const uint8_t *buf, unsigned size,
                       const kcpmux_addr_t *peer_addr)
{
    if (!engine || !buf || size == 0 || !peer_addr || !peer_addr->addr) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_engine_operation_enter(engine);

    // Update rx statistics
    engine->stats.rx_packets++;
    engine->stats.rx_bytes += size;

    // Find or create connection to handle packet
    // Implementation is in kcpmux_protocol.c
    int ret = kcpmux_protocol_input(
        engine, buf, size, peer_addr, kcpmux_engine_now(engine));
    kcpmux_engine_operation_leave(engine);
    return ret;
}

// ============================================================================
// Engine helper functions
// ============================================================================

int64_t kcpmux_engine_now(kcpmux_engine_t *engine) {
    return engine ? engine->callbacks.monotonic_time_ms(engine->user_data) : 0;
}

void kcpmux_engine_operation_enter(kcpmux_engine_t *engine) {
    if (engine) {
        engine->operation_depth++;
    }
}

void kcpmux_engine_operation_leave(kcpmux_engine_t *engine) {
    if (!engine || engine->operation_depth == 0) {
        return;
    }
    engine->operation_depth--;
    if (engine->operation_depth != 0) {
        return;
    }
    // Drain releases inside the operation boundary so release hooks cannot
    // expose partially drained ownership state.
    engine->operation_depth = 1;
    kcpmux_engine_drain_pending_release(engine);
    engine->operation_depth = 0;
    kcpmux_engine_rearm_timer(engine, kcpmux_engine_now(engine));
}

void kcpmux_engine_queue_release(kcpmux_engine_t *engine,
                                kcpmux_pending_release_t *item,
                                kcpmux_release_cb release_cb) {
    if (!engine || !item || !release_cb) {
        return;
    }
    item->release_cb = release_cb;
    list_add_tail(&item->node, &engine->pending_release_list);
}

int kcpmux_engine_register_timer_node(kcpmux_engine_t *engine,
                                     kcpmux_timer_node_t *node,
                                     void *owner,
                                     kcpmux_timer_cb timeout_cb) {
    int ret;
    size_t required_capacity;

    if (!engine || !node || !owner || !timeout_cb) {
        return KCPMUX_ERR_INVALID_PARAM;
    }
    if (engine->timer_node_count > SIZE_MAX - 2) {
        return KCPMUX_ERR_OOM;
    }
    // Scheduling a successfully registered owner must not be the first
    // operation that needs to grow the heap.
    required_capacity = engine->timer_node_count + 1;
    ret = kcpmux_timer_manager_reserve(&engine->timer_manager,
                                      required_capacity);
    if (ret != KCPMUX_ERR_OK) {
        return ret;
    }
    kcpmux_timer_node_init(node, owner, timeout_cb);
    engine->timer_node_count++;
    return KCPMUX_ERR_OK;
}

void kcpmux_engine_unregister_timer_node(kcpmux_engine_t *engine,
                                        kcpmux_timer_node_t *node) {
    int64_t now;

    if (!engine || !node || !node->owner) {
        return;
    }
    now = kcpmux_engine_now(engine);
    kcpmux_timer_cancel(&engine->timer_manager, node);
    node->owner = NULL;
    node->timeout_cb = NULL;
    if (engine->timer_node_count > 0) {
        engine->timer_node_count--;
    }
    kcpmux_engine_rearm_timer(engine, now);
}

int kcpmux_engine_schedule_timer_node(kcpmux_engine_t *engine,
                                     kcpmux_timer_node_t *node,
                                     int64_t deadline_ms,
                                     int64_t now_ms) {
    int ret;

    if (!engine || !node || !node->owner) {
        return KCPMUX_ERR_INVALID_PARAM;
    }
    if (deadline_ms < now_ms) {
        deadline_ms = now_ms;
    }
    ret = kcpmux_timer_schedule(&engine->timer_manager, node, deadline_ms);
    if (ret == KCPMUX_ERR_OK) {
        kcpmux_engine_rearm_timer(engine, now_ms);
    }
    return ret;
}

void kcpmux_engine_cancel_timer_node(kcpmux_engine_t *engine,
                                    kcpmux_timer_node_t *node,
                                    int64_t now_ms) {
    if (!engine || !node) {
        return;
    }
    kcpmux_timer_cancel(&engine->timer_manager, node);
    kcpmux_engine_rearm_timer(engine, now_ms);
}

void kcpmux_engine_rearm_timer(kcpmux_engine_t *engine, int64_t now_ms) {
    kcpmux_timer_node_t *root;
    int64_t deadline_ms;
    uint64_t delay_ms;

    if (!engine || !engine->callbacks.set_timer || engine->operation_depth > 0) {
        return;
    }

    root = kcpmux_timer_peek(&engine->timer_manager);
    if (!root) {
        // There is no cancel callback. The previous one-shot may fire once and
        // will observe an empty heap without rearming.
        return;
    }
    deadline_ms = root->deadline_ms;
    if (engine->external_timer_armed &&
        engine->external_timer_deadline_ms == deadline_ms) {
        return;
    }

    delay_ms = deadline_ms <= now_ms
        ? 0
        : (uint64_t)deadline_ms - (uint64_t)now_ms;
    // Publish the requested one-shot before invoking the host callback.
    engine->external_timer_armed = 1;
    engine->external_timer_deadline_ms = deadline_ms;
    engine->callbacks.set_timer(delay_ms, engine->user_data);
}

int kcpmux_engine_write_socket(kcpmux_engine_t *engine,
                              const uint8_t *buf, unsigned size,
                              const kcpmux_addr_t *addr)
{
    if (!engine || !engine->callbacks.write_socket) return -KCPMUX_ERR_INVALID_PARAM;

    int ret = engine->callbacks.write_socket(buf, size, addr, engine->user_data);

    // Update tx statistics
    if (ret) {
        engine->stats.tx_packets++;
        engine->stats.tx_bytes += size;
    } else {
        engine->stats.tx_error_packets++;
    }

    return ret ? 0 : -KCPMUX_ERR_NETWORK;
}

void kcpmux_engine_add_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn) {
    if (!engine || !conn || conn->internal_closed || conn->in_engine_map) return;

    uint32_t hash = kcpmux_hash32(conn->peer_addr.addr, conn->peer_addr.addrlen);
    kcpmux_htb_add(engine->conn_map, &conn->hash_node, &conn->peer_addr, hash);
    engine->conn_count++;
    conn->in_engine_map = 1;

    // Update engine stats
    engine->stats.conn_count = engine->conn_count;
}

void kcpmux_engine_remove_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn) {
    if (!engine || !conn || !conn->in_engine_map) return;

    kcpmux_htb_del(engine->conn_map, &conn->hash_node);
    engine->conn_count--;
    conn->in_engine_map = 0;

    // Update engine stats
    engine->stats.conn_count = engine->conn_count;
}

kcpmux_conn_t *kcpmux_engine_get_conn_by_addr(
    kcpmux_engine_t *engine,
    const kcpmux_addr_t *addr)
{
    if (!engine || !addr || !addr->addr) return NULL;

    uint32_t hash = kcpmux_hash32(addr->addr, addr->addrlen);
    list_head *node = kcpmux_htb_find(engine->conn_map, (void *)addr, hash);
    if (!node) return NULL;

    return list_entry(node, kcpmux_conn_t, hash_node);
}

void kcpmux_engine_get_stats(kcpmux_engine_t *engine, kcpmux_engine_stats_t *stats) {
    if (!stats) return;

    if (!engine) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    memcpy(stats, &engine->stats, sizeof(*stats));
}
