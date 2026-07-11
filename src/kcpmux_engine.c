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
        free(engine);
        return NULL;
    }

    // Schedule first timer
    kcpmux_engine_schedule_timer(engine, kcpmux_engine_now(engine));

    return engine;
}

void kcpmux_engine_destroy(kcpmux_engine_t *engine) {
    if (!engine) return;

    // Free all connections
    // Iterate hash table, free each connection
    // Note: conn_free will auto-close and trigger stream and conn close_notify
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
            kcpmux_conn_free(conn);
        } else {
            break;
        }
    }

    // Destroy hash table
    kcpmux_htb_destroy(engine->conn_map);

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

    // Update all connections
    for (int i = 0; i < engine->conn_map->size; i++) {
        list_head *pos, *tmp;
        list_for_each_safe(pos, tmp, &engine->conn_map->hashtable[i]) {
            kcpmux_conn_t *conn = list_entry(pos, kcpmux_conn_t, hash_node);
            kcpmux_conn_update(conn, now);
        }
    }

    // Schedule next timer
    kcpmux_engine_schedule_timer(engine, now);
}

int kcpmux_engine_input(kcpmux_engine_t *engine,
                       const uint8_t *buf, unsigned size,
                       const kcpmux_addr_t *peer_addr)
{
    if (!engine || !buf || size == 0 || !peer_addr || !peer_addr->addr) return -KCPMUX_ERR_INVALID_PARAM;

    // Update rx statistics
    engine->stats.rx_packets++;
    engine->stats.rx_bytes += size;

    // Find or create connection to handle packet
    // Implementation is in kcpmux_protocol.c
    return kcpmux_protocol_input(engine, buf, size, peer_addr, kcpmux_engine_now(engine));
}

// ============================================================================
// Engine helper functions
// ============================================================================

int64_t kcpmux_engine_now(kcpmux_engine_t *engine) {
    return engine ? engine->callbacks.monotonic_time_ms(engine->user_data) : 0;
}

void kcpmux_engine_schedule_timer(kcpmux_engine_t *engine, int64_t now) {
    if (!engine || !engine->callbacks.set_timer) return;

    // Calculate next wakeup time (default 10ms)
    int64_t min_interval = 10;

    // Iterate all connections, get minimum wakeup interval
    for (int i = 0; i < engine->conn_map->size; i++) {
        list_head *pos;
        list_for_each(pos, &engine->conn_map->hashtable[i]) {
            kcpmux_conn_t *conn = list_entry(pos, kcpmux_conn_t, hash_node);
            int64_t conn_interval = kcpmux_conn_check_interval(conn, now);
            if (conn_interval > 0 && conn_interval < min_interval) {
                min_interval = conn_interval;
            }
        }
    }

    engine->callbacks.set_timer((uint64_t)min_interval, engine->user_data);
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
    if (!engine || !conn) return;

    uint32_t hash = kcpmux_hash32(conn->peer_addr.addr, conn->peer_addr.addrlen);
    kcpmux_htb_add(engine->conn_map, &conn->hash_node, &conn->peer_addr, hash);
    engine->conn_count++;

    // Update engine stats
    engine->stats.conn_count = engine->conn_count;
}

void kcpmux_engine_remove_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn) {
    if (!engine || !conn) return;

    kcpmux_htb_del(engine->conn_map, &conn->hash_node);
    engine->conn_count--;

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
