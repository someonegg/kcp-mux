#include "kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_stream.h"
#include "kcpmux_protocol.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Hash table helper functions
// ============================================================================

// Returns: 1 if match, 0 if not match
static int __stream_cmp(void *key, list_head *entry) {
    uint32_t stream_id = *(uint32_t *)key;
    kcpmux_stream_t *stream = list_entry(entry, kcpmux_stream_t, hash_node);
    return stream_id == stream->stream_id;
}

static void __stream_free(list_head *entry) {
    (void)entry;
}

// ============================================================================
// Connection lifecycle
// ============================================================================

kcpmux_conn_t *kcpmux_conn_new(kcpmux_engine_t *engine,
                             const kcpmux_addr_t *peer_addr,
                             const kcpmux_conn_config_t *config,
                             uint8_t is_initiator)
{
    if (!engine || !peer_addr || !peer_addr->addr || peer_addr->addrlen == 0) {
        return NULL;
    }
    if (peer_addr->addrlen > KCPMUX_ADDR_MAX_LEN) {
        return NULL;
    }

    kcpmux_conn_t *conn = (kcpmux_conn_t *)malloc(sizeof(kcpmux_conn_t));
    if (!conn) return NULL;

    memset(conn, 0, sizeof(*conn));

    conn->state = KCPMUX_CONN_STATE_INIT;
    conn->is_initiator = is_initiator;
    conn->engine = engine;

    // Copy peer address to internal buffer
    memcpy(conn->peer_addr_buf, peer_addr->addr, peer_addr->addrlen);
    conn->peer_addr.addr = conn->peer_addr_buf;
    conn->peer_addr.addrlen = peer_addr->addrlen;

    // Config
    if (config) {
        conn->config = *config;
    } else {
        // Use engine's default conn config
        conn->config = engine->default_conn_config;
    }

    // stream_id allocation rule: initiator uses odd, acceptor uses even
    // Use random start value for better security
    {
        uint32_t random_index = (uint32_t)(rand() % KCPMUX_STREAM_ID_MAX) / 2;

        // It's ok even if next_stream_id > KCPMUX_STREAM_ID_MAX, see kcpmux_conn_alloc_stream_id.
        conn->next_stream_id = is_initiator
            ? (random_index * 2 + 1)   // odd: 1, 3, 5, ...
            : (random_index * 2 + 2);  // even: 2, 4, 6, ...
    }

    // Create stream hash table
    conn->stream_map = kcpmux_htb_new(16, __stream_cmp, __stream_free);
    if (!conn->stream_map) {
        free(conn);
        return NULL;
    }

    // Initialize timestamps
    int64_t now = kcpmux_engine_now(engine);
    conn->created_ts = now;
    conn->last_recv_ts = now;
    conn->last_keepalive_ts = now;
    conn->last_payload_ts = now;

    // Initialize list node
    INIT_LIST_HEAD(&conn->hash_node);

    // Update statistics
    conn->engine->stats.conn_created_total++;

    return conn;
}

void kcpmux_conn_close_internal(kcpmux_conn_t *conn, uint8_t reason) {
    if (!conn) return;

    // Skip if already closed
    if (conn->internal_closed) {
        return;
    } else {
        conn->internal_closed = 1;
    }

    // Update statistics
    conn->engine->stats.conn_closed_total++;

    // Close all streams first (cascade close)
    for (int i = 0; i < conn->stream_map->size; i++) {
        list_head *pos, *tmp;
        list_for_each_safe(pos, tmp, &conn->stream_map->hashtable[i]) {
            kcpmux_stream_t *stream = list_entry(pos, kcpmux_stream_t, hash_node);
            kcpmux_stream_close_internal(stream, reason);
        }
    }

    // Set state before callback (so callback can query final state)
    if (conn->state != KCPMUX_CONN_STATE_ERROR) {
        conn->state = KCPMUX_CONN_STATE_CLOSED;
    }

    // Trigger close callback
    if (conn->callbacks.conn_close_notify) {
        conn->callbacks.conn_close_notify(conn, reason, conn->user_data);
    }
}

void kcpmux_conn_free(kcpmux_conn_t *conn) {
    if (!conn) return;

    // Auto close if not closed
    kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_NORMAL);

    // Cascade free all streams (no callback since already closed)
    while (conn->stream_count > 0) {
        list_head *node = NULL;
        for (int i = 0; i < conn->stream_map->size && !node; i++) {
            if (!list_empty(&conn->stream_map->hashtable[i])) {
                node = conn->stream_map->hashtable[i].next;
            }
        }
        if (node) {
            kcpmux_stream_t *stream = list_entry(node, kcpmux_stream_t, hash_node);
            kcpmux_stream_free(stream);
        } else {
            break;
        }
    }

    // Destroy stream hash table
    kcpmux_htb_destroy(conn->stream_map);

    // Remove from engine
    kcpmux_engine_remove_conn(conn->engine, conn);

    free(conn);
}

// ============================================================================
// Connection public API
// ============================================================================

kcpmux_conn_t *kcpmux_conn_connect(
    kcpmux_engine_t *engine,
    const kcpmux_addr_t *peer_addr,
    const kcpmux_conn_config_t *config,
    const kcpmux_proto_ext_t *proto_ext,
    const kcpmux_conn_callbacks_t *callbacks,
    void *user_data)
{
    if (!engine || !peer_addr || !peer_addr->addr) return NULL;

    // Update API call statistics
    engine->stats.api_conn_connect_calls++;

    // Check if connection already exists
    if (kcpmux_engine_get_conn_by_addr(engine, peer_addr)) {
        return NULL;
    }

    // Create connection
    kcpmux_conn_t *conn = kcpmux_conn_new(engine, peer_addr, config, 1);
    if (!conn) return NULL;

    // Set callbacks
    if (callbacks) {
        conn->callbacks = *callbacks;
    }
    conn->user_data = user_data;

    // Set self protocol extension data
    if (proto_ext && proto_ext->data && proto_ext->len > 0) {
        unsigned len = proto_ext->len;
        if (len > KCPMUX_PROTO_EXT_MAX_LEN) {
            len = KCPMUX_PROTO_EXT_MAX_LEN;
        }
        memcpy(conn->self_proto_ext_buf, proto_ext->data, len);
        conn->self_proto_ext.data = conn->self_proto_ext_buf;
        conn->self_proto_ext.len = len;
    }

    // Add to engine
    kcpmux_engine_add_conn(engine, conn);

    // Send connection request
    kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTING);
    kcpmux_conn_send_connect(conn);

    return conn;
}

int kcpmux_conn_close(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // Update API call statistics
    conn->engine->stats.api_conn_close_calls++;

    if (conn->state == KCPMUX_CONN_STATE_CLOSING ||
        conn->state == KCPMUX_CONN_STATE_CLOSED ||
        conn->state == KCPMUX_CONN_STATE_ERROR) {
        return 0;
    }

    // Enter closing state
    kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CLOSING);
    conn->close_ts = kcpmux_engine_now(conn->engine);
    conn->close_reason = KCPMUX_CLOSE_REASON_NORMAL;
    conn->retry_count = 0;

    // Send close message
    kcpmux_conn_send_close(conn, KCPMUX_CLOSE_REASON_NORMAL);

    return 0;
}

void kcpmux_conn_set_config(kcpmux_conn_t *conn,
                           const kcpmux_conn_config_t *config)
{
    if (!conn || !config || conn->internal_closed) return;

    // Directly replace the entire config structure
    conn->config = *config;
}

void kcpmux_conn_set_callbacks(kcpmux_conn_t *conn,
                              const kcpmux_conn_callbacks_t *callbacks,
                              void *user_data)
{
    if (!conn || conn->internal_closed) return;

    if (callbacks) {
        conn->callbacks = *callbacks;
    }
    conn->user_data = user_data;
}

// ============================================================================
// Connection query API
// ============================================================================

uint8_t kcpmux_conn_get_state(kcpmux_conn_t *conn) {
    return conn ? conn->state : KCPMUX_CONN_STATE_ERROR;
}

kcpmux_engine_t *kcpmux_conn_get_engine(kcpmux_conn_t *conn) {
    return conn ? conn->engine : NULL;
}

const kcpmux_addr_t *kcpmux_conn_get_peer_addr(kcpmux_conn_t *conn) {
    return conn ? &conn->peer_addr : NULL;
}

void *kcpmux_conn_get_user_data(kcpmux_conn_t *conn) {
    return conn ? conn->user_data : NULL;
}

const kcpmux_proto_ext_t *kcpmux_conn_get_peer_proto_ext(kcpmux_conn_t *conn) {
    if (!conn || conn->peer_proto_ext.len == 0) return NULL;
    return &conn->peer_proto_ext;
}

const kcpmux_proto_ext_t *kcpmux_conn_get_self_proto_ext(kcpmux_conn_t *conn) {
    if (!conn || conn->self_proto_ext.len == 0) return NULL;
    return &conn->self_proto_ext;
}

void kcpmux_conn_get_stats(kcpmux_conn_t *conn, kcpmux_conn_stats_t *stats) {
    if (!stats) return;

    if (!conn) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    memcpy(stats, &conn->stats, sizeof(*stats));
}

// ============================================================================
// Connection internal functions
// ============================================================================

void kcpmux_conn_set_state(kcpmux_conn_t *conn, uint8_t new_state) {
    if (!conn || conn->state == new_state) return;

    uint8_t old_state = conn->state;
    conn->state = new_state;

    // Calculate handshake time when transitioning to CONNECTED state
    if (new_state == KCPMUX_CONN_STATE_CONNECTED) {
        int64_t now = kcpmux_engine_now(conn->engine);
        conn->stats.handshake_time_ms = (uint64_t)(now - conn->created_ts);
        // Update statistics
        conn->engine->stats.conn_connected_total++;
    }

    if (conn->callbacks.conn_state_changed) {
        conn->callbacks.conn_state_changed(conn, old_state, new_state, conn->user_data);
    }
}

void kcpmux_conn_update(kcpmux_conn_t *conn, int64_t now) {
    if (!conn) return;

    // Check if it's time to update
    if (conn->next_update_ts > 0 && now < conn->next_update_ts) {
        return;
    }

    switch (conn->state) {
    case KCPMUX_CONN_STATE_CONNECTING:
        // Check connect timeout and retransmit
        if (now - conn->last_ctrl_ts >= conn->config.ctrl_timeout_ms) {
            if (conn->retry_count < conn->config.connect_retries) {
                conn->retry_count++;
                kcpmux_conn_send_connect(conn);
            } else {
                kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_ERROR);
                kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_TIMEOUT);
                return;
            }
        }
        break;

    case KCPMUX_CONN_STATE_CONNECTED:
        // Check keepalive timeout
        if (now - conn->last_recv_ts >= conn->config.keepalive_timeout_ms) {
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_ERROR);
            // Update statistics
            conn->engine->stats.conn_keepalive_timeout_total++;
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_TIMEOUT);
            return;
        }

        // Check idle timeout (no stream_payload received)
        if (conn->config.idle_timeout_ms > 0 &&
            now - conn->last_payload_ts >= conn->config.idle_timeout_ms) {
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CLOSING);
            kcpmux_conn_send_close(conn, KCPMUX_CLOSE_REASON_IDLE);
            // Update statistics
            conn->engine->stats.conn_idle_timeout_total++;
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_IDLE);
            return;
        }

        // Send keepalive
        if (now - conn->last_keepalive_ts >= conn->config.keepalive_interval_ms) {
            kcpmux_conn_send_keepalive(conn);
            conn->last_keepalive_ts = now;
        }

        // Update all streams
        for (int i = 0; i < conn->stream_map->size; i++) {
            list_head *pos, *tmp;
            list_for_each_safe(pos, tmp, &conn->stream_map->hashtable[i]) {
                kcpmux_stream_t *stream = list_entry(pos, kcpmux_stream_t, hash_node);
                kcpmux_stream_update(stream, now);
            }
        }
        break;

    case KCPMUX_CONN_STATE_CLOSING:
        // Check close timeout and retransmit
        if (now - conn->last_ctrl_ts >= conn->config.ctrl_timeout_ms) {
            if (conn->retry_count < conn->config.close_retries) {
                conn->retry_count++;
                kcpmux_conn_send_close(conn, conn->close_reason);
            } else {
                kcpmux_conn_close_internal(conn, conn->close_reason);
                return;
            }
        }
        break;

    default:
        break;
    }
}

int64_t kcpmux_conn_check_interval(kcpmux_conn_t *conn, int64_t now) {
    if (!conn) return 10;

    int64_t interval = 10; // Default 10ms

    switch (conn->state) {
    case KCPMUX_CONN_STATE_CONNECTING:
        interval = conn->config.ctrl_timeout_ms;
        break;

    case KCPMUX_CONN_STATE_CONNECTED:
        interval = conn->config.keepalive_interval_ms;
        // Check stream intervals
        for (int i = 0; i < conn->stream_map->size; i++) {
            list_head *pos;
            list_for_each(pos, &conn->stream_map->hashtable[i]) {
                kcpmux_stream_t *stream = list_entry(pos, kcpmux_stream_t, hash_node);
                int64_t s_interval = kcpmux_stream_check_interval(stream, now);
                if (s_interval < interval) {
                    interval = s_interval;
                }
            }
        }
        break;

    case KCPMUX_CONN_STATE_CLOSING:
        interval = conn->config.ctrl_timeout_ms;
        break;

    default:
        break;
    }

    // Set next update timestamp
    conn->next_update_ts = now + interval;

    return interval;
}

// ============================================================================
// Stream management
// ============================================================================

void kcpmux_conn_add_stream(kcpmux_conn_t *conn, kcpmux_stream_t *stream) {
    if (!conn || !stream) return;

    uint32_t hash = stream->stream_id;
    kcpmux_htb_add(conn->stream_map, &stream->hash_node, &stream->stream_id, hash);
    conn->stream_count++;

    // Update engine stats
    if (conn->engine) {
        conn->engine->stats.stream_count++;
    }
}

void kcpmux_conn_remove_stream(kcpmux_conn_t *conn, kcpmux_stream_t *stream) {
    if (!conn || !stream) return;

    kcpmux_htb_del(conn->stream_map, &stream->hash_node);
    conn->stream_count--;

    // Update engine stats
    if (conn->engine) {
        conn->engine->stats.stream_count--;
    }
}

kcpmux_stream_t *kcpmux_conn_get_stream_by_id(kcpmux_conn_t *conn, uint32_t stream_id) {
    if (!conn) return NULL;

    uint32_t hash = stream_id;
    list_head *node = kcpmux_htb_find(conn->stream_map, &stream_id, hash);
    if (!node) return NULL;

    return list_entry(node, kcpmux_stream_t, hash_node);
}

uint32_t kcpmux_conn_alloc_stream_id(kcpmux_conn_t *conn) {
    if (!conn) return 0;

    if (conn->next_stream_id > KCPMUX_STREAM_ID_MAX) {
        // Wrap around to starting value
        conn->next_stream_id = (conn->is_initiator ? 1 : 2);
    }

    uint32_t id = conn->next_stream_id;
    conn->next_stream_id += 2;
    return id;
}

// ============================================================================
// Message send (implemented in kcpmux_protocol.c)
// ============================================================================

int kcpmux_conn_send_connect(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;
    conn->last_ctrl_ts = kcpmux_engine_now(conn->engine);
    return kcpmux_protocol_send_conn_connect(conn);
}

int kcpmux_conn_send_connect_ack(kcpmux_conn_t *conn, uint8_t result) {
    return kcpmux_protocol_send_conn_connect_ack(conn, result);
}

int kcpmux_conn_send_keepalive(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;
    conn->keepalive_seq++;
    return kcpmux_protocol_send_conn_keepalive(conn);
}

int kcpmux_conn_send_close(kcpmux_conn_t *conn, uint8_t reason) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;
    conn->last_ctrl_ts = kcpmux_engine_now(conn->engine);
    return kcpmux_protocol_send_conn_close(conn, reason);
}

int kcpmux_conn_send_close_ack(kcpmux_conn_t *conn, uint8_t reason) {
    return kcpmux_protocol_send_conn_close_ack(conn, reason);
}

int kcpmux_conn_write_socket(kcpmux_conn_t *conn, const uint8_t *buf, unsigned size) {
    int ret = kcpmux_engine_write_socket(conn->engine, buf, size, &conn->peer_addr);
    if (ret == 0) {
        conn->stats.tx_packets++;
        conn->stats.tx_bytes += size;
    }
    return ret;
}
