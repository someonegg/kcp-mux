#include "kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_stream.h"
#include "kcpmux_protocol.h"
#include "kcpmux_hash.h"

#include <stdlib.h>
#include <string.h>

static void kcpmux_conn_timer_callback(kcpmux_timer_node_t *node, int64_t now_ms);

static void kcpmux_conn_release(kcpmux_pending_release_t *item)
{
    kcpmux_conn_t *conn = list_entry(item, kcpmux_conn_t, pending_release);
    kcpmux_htb_destroy(conn->stream_map);
    free(conn);
}

static int kcpmux_conn_calculate_deadline(kcpmux_conn_t *conn, int64_t *deadline_ms)
{
    int64_t deadline;
    int64_t candidate;

    if (!conn || !deadline_ms || conn->internal_closed) {
        return 0;
    }

    switch (conn->state) {
    case KCPMUX_CONN_STATE_CONNECTING:
    case KCPMUX_CONN_STATE_CLOSING:
        deadline = kcpmux_timer_deadline_after(conn->last_ctrl_ts, conn->config.ctrl_timeout_ms);
        break;

    case KCPMUX_CONN_STATE_CONNECTED:
        deadline = kcpmux_timer_deadline_after(
            conn->last_recv_ts,
            conn->config.keepalive_timeout_ms);
        if (conn->config.keepalive_interval_ms > 0) {
            candidate = kcpmux_timer_deadline_after(
                conn->last_keepalive_ts,
                conn->config.keepalive_interval_ms);
            if (candidate < deadline) {
                deadline = candidate;
            }
        }
        if (conn->config.idle_timeout_ms > 0) {
            candidate = kcpmux_timer_deadline_after(
                conn->last_payload_ts,
                conn->config.idle_timeout_ms);
            if (candidate < deadline) {
                deadline = candidate;
            }
        }
        break;

    default:
        deadline = INT64_MAX;
        break;
    }

    if (deadline == INT64_MAX) {
        return 0;
    }
    *deadline_ms = deadline;
    return 1;
}

// ============================================================================
// Hash table helper functions
// ============================================================================

// Returns: 1 if match, 0 if not match
static int __stream_cmp(void *key, list_head *entry)
{
    uint32_t stream_id = *(uint32_t *)key;
    kcpmux_stream_t *stream = list_entry(entry, kcpmux_stream_t, hash_node);
    return stream_id == stream->stream_id;
}

static void __stream_free(list_head *entry)
{
    (void)entry;
}

// ============================================================================
// Connection lifecycle
// ============================================================================

kcpmux_conn_t *kcpmux_conn_new(
    kcpmux_engine_t *engine,
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
    INIT_LIST_HEAD(&conn->pending_release.node);

    conn->state = KCPMUX_CONN_STATE_INIT;
    conn->is_initiator = is_initiator;
    conn->generation_id = (uint32_t)rand() & 0x00FFFFFFU;
    if (conn->generation_id == 0) {
        conn->generation_id = 1;
    }
    conn->engine = engine;

    // Copy peer address to internal buffer
    memcpy(conn->peer_addr_buf, peer_addr->addr, peer_addr->addrlen);
    conn->peer_addr.addr = conn->peer_addr_buf;
    conn->peer_addr.addrlen = peer_addr->addrlen;

    // Config
    const kcpmux_conn_config_t *source = config ? config : &engine->default_conn_config;
    if (!kcpmux_conn_config_prepare(&conn->config, source)) {
        free(conn);
        return NULL;
    }

    // Initiators use odd stream IDs and acceptors use even stream IDs.
    conn->next_stream_id = (uint32_t)rand();
    if (is_initiator) {
        conn->next_stream_id |= 1U;
    } else {
        conn->next_stream_id &= ~1U;
        if (conn->next_stream_id == 0) {
            conn->next_stream_id = 2;
        }
    }

    // Create stream hash table
    conn->stream_map = kcpmux_htb_new(32, __stream_cmp, __stream_free);
    if (!conn->stream_map) {
        free(conn);
        return NULL;
    }

    if (kcpmux_engine_register_timer_node(
            engine,
            &conn->timer_node,
            conn,
            kcpmux_conn_timer_callback) != KCPMUX_ERR_OK) {
        kcpmux_htb_destroy(conn->stream_map);
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

void kcpmux_conn_close_internal(kcpmux_conn_t *conn, uint8_t reason)
{
    if (!conn) return;

    // Skip if already closed
    if (conn->internal_closed) {
        return;
    } else {
        conn->internal_closed = 1;
    }

    kcpmux_engine_t *engine = conn->engine;
    kcpmux_engine_unregister_timer_node(engine, &conn->timer_node);
    kcpmux_engine_remove_conn(engine, conn);

    // Update statistics
    engine->stats.conn_closed_total++;

    // Close all streams before notifying that the connection has closed. Each
    // stream removes its own hash node, so use the standard safe iterator.
    for (int i = 0; i < conn->stream_map->size; i++) {
        list_head *pos;
        list_head *next;
        list_for_each_safe(pos, next, &conn->stream_map->hashtable[i]) {
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

    // Streams queued themselves during the cascade, so the connection is
    // always released after every owned stream.
    kcpmux_engine_queue_release(engine, &conn->pending_release, kcpmux_conn_release);
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

    kcpmux_engine_operation_enter(engine);

    // Update API call statistics
    engine->stats.api_conn_connect_calls++;

    // A healthy or connecting connection remains unique per address. A
    // lingering CLOSING generation may be replaced immediately.
    kcpmux_conn_t *old_conn = kcpmux_engine_get_conn_by_addr(engine, peer_addr);
    if (old_conn && old_conn->state != KCPMUX_CONN_STATE_CLOSING) {
        kcpmux_engine_operation_leave(engine);
        return NULL;
    }

    // Create connection
    kcpmux_conn_t *conn = kcpmux_conn_new(engine, peer_addr, config, 1);
    if (!conn) {
        kcpmux_engine_operation_leave(engine);
        return NULL;
    }

    // Set callbacks
    if (callbacks) {
        conn->callbacks = *callbacks;
    }
    conn->user_data = user_data;

    // A generation identifies the address mapping on the wire. Ensure a
    // locally replaced generation can never collide with the closing one.
    if (old_conn && conn->generation_id == old_conn->generation_id) {
        conn->generation_id = old_conn->generation_id == 0x00FFFFFFU
            ? 1U
            : old_conn->generation_id + 1U;
    }

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

    // The replacement is fully initialized before the old close cascade. The
    // close notification is synchronous, while physical release stays deferred
    // until this public operation leaves.
    if (old_conn) {
        kcpmux_conn_close_internal(old_conn, KCPMUX_CLOSE_REASON_REPLACED);
    }

    // Add to engine
    kcpmux_engine_add_conn(engine, conn);

    // Anchor the first control deadline before exposing CONNECTING state.
    conn->last_ctrl_ts = kcpmux_engine_now(engine);
    kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTING);
    kcpmux_conn_send_connect(conn);

    kcpmux_engine_operation_leave(engine);
    return conn;
}

int kcpmux_conn_close(kcpmux_conn_t *conn)
{
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_engine_t *engine = conn->engine;
    kcpmux_engine_operation_enter(engine);

    // Update API call statistics
    conn->engine->stats.api_conn_close_calls++;

    if (conn->internal_closed) {
        kcpmux_engine_operation_leave(engine);
        return 0;
    }

    if (conn->state == KCPMUX_CONN_STATE_CLOSING ||
        conn->state == KCPMUX_CONN_STATE_CLOSED ||
        conn->state == KCPMUX_CONN_STATE_ERROR) {
        kcpmux_engine_operation_leave(engine);
        return 0;
    }

    // Initialize all closing anchors before exposing the state transition.
    conn->last_ctrl_ts = kcpmux_engine_now(conn->engine);
    conn->close_reason = KCPMUX_CLOSE_REASON_NORMAL;
    conn->retry_count = 0;
    kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CLOSING);

    // Send close message
    kcpmux_conn_send_close(conn, KCPMUX_CLOSE_REASON_NORMAL);
    if (conn->config.close_retries == 0) {
        kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_NORMAL);
    }

    kcpmux_engine_operation_leave(engine);
    return 0;
}

void kcpmux_conn_set_config(kcpmux_conn_t *conn, const kcpmux_conn_config_t *config)
{
    if (!conn || !config || conn->internal_closed) return;

    kcpmux_conn_config_t prepared;
    if (!kcpmux_conn_config_prepare(&prepared, config)) return;
    conn->config = prepared;
    kcpmux_conn_refresh_timer(conn, kcpmux_engine_now(conn->engine));
}

void kcpmux_conn_set_callbacks(
    kcpmux_conn_t *conn,
    const kcpmux_conn_callbacks_t *callbacks,
    void *user_data)
{
    if (!conn || conn->internal_closed) return;

    if (callbacks) {
        conn->callbacks = *callbacks;
    } else {
        memset(&conn->callbacks, 0, sizeof(conn->callbacks));
    }
    conn->user_data = user_data;
}

// ============================================================================
// Connection query API
// ============================================================================

uint8_t kcpmux_conn_get_state(kcpmux_conn_t *conn)
{
    return conn ? conn->state : KCPMUX_CONN_STATE_ERROR;
}

kcpmux_engine_t *kcpmux_conn_get_engine(kcpmux_conn_t *conn)
{
    return conn ? conn->engine : NULL;
}

const kcpmux_addr_t *kcpmux_conn_get_peer_addr(kcpmux_conn_t *conn)
{
    return conn ? &conn->peer_addr : NULL;
}

void *kcpmux_conn_get_user_data(kcpmux_conn_t *conn)
{
    return conn ? conn->user_data : NULL;
}

const kcpmux_proto_ext_t *kcpmux_conn_get_peer_proto_ext(kcpmux_conn_t *conn)
{
    if (!conn || conn->peer_proto_ext.len == 0) return NULL;
    return &conn->peer_proto_ext;
}

const kcpmux_proto_ext_t *kcpmux_conn_get_self_proto_ext(kcpmux_conn_t *conn)
{
    if (!conn || conn->self_proto_ext.len == 0) return NULL;
    return &conn->self_proto_ext;
}

void kcpmux_conn_get_stats(kcpmux_conn_t *conn, kcpmux_conn_stats_t *stats)
{
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

void kcpmux_conn_set_state(kcpmux_conn_t *conn, uint8_t new_state)
{
    if (!conn || conn->internal_closed || conn->state == new_state) return;

    uint8_t old_state = conn->state;
    conn->state = new_state;

    // Calculate handshake time when transitioning to CONNECTED state
    if (new_state == KCPMUX_CONN_STATE_CONNECTED) {
        int64_t now = kcpmux_engine_now(conn->engine);
        conn->stats.handshake_time_ms = (uint64_t)(now - conn->created_ts);
        // Update statistics
        conn->engine->stats.conn_connected_total++;
    }

    kcpmux_conn_refresh_timer(conn, kcpmux_engine_now(conn->engine));

    if (conn->callbacks.conn_state_changed) {
        conn->callbacks.conn_state_changed(conn, old_state, new_state, conn->user_data);
    }
}

void kcpmux_conn_update(kcpmux_conn_t *conn, int64_t now)
{
    int64_t deadline_ms;

    if (!conn || conn->internal_closed) return;

    if (!kcpmux_conn_calculate_deadline(conn, &deadline_ms) ||
        deadline_ms > now) {
        kcpmux_conn_refresh_timer(conn, now);
        return;
    }

    switch (conn->state) {
    case KCPMUX_CONN_STATE_CONNECTING:
        // Check connect timeout and retransmit
        if (now >= kcpmux_timer_deadline_after(conn->last_ctrl_ts, conn->config.ctrl_timeout_ms)) {
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
        if (now >=
            kcpmux_timer_deadline_after(conn->last_recv_ts, conn->config.keepalive_timeout_ms)) {
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_ERROR);
            // Update statistics
            conn->engine->stats.conn_keepalive_timeout_total++;
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_TIMEOUT);
            return;
        }

        // Check idle timeout (no stream_payload received)
        if (conn->config.idle_timeout_ms > 0 &&
            now >=
                kcpmux_timer_deadline_after(conn->last_payload_ts, conn->config.idle_timeout_ms)) {
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CLOSING);
            kcpmux_conn_send_close(conn, KCPMUX_CLOSE_REASON_IDLE);
            // Update statistics
            conn->engine->stats.conn_idle_timeout_total++;
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_IDLE);
            return;
        }

        // Send keepalive
        if (conn->config.keepalive_interval_ms > 0 &&
            now >= kcpmux_timer_deadline_after(
                       conn->last_keepalive_ts,
                       conn->config.keepalive_interval_ms)) {
            kcpmux_conn_send_keepalive(conn);
        }
        break;

    case KCPMUX_CONN_STATE_CLOSING:
        // Check close timeout and retransmit
        if (now >= kcpmux_timer_deadline_after(conn->last_ctrl_ts, conn->config.ctrl_timeout_ms)) {
            if (conn->retry_count < conn->config.close_retries) {
                conn->retry_count++;
                kcpmux_conn_send_close(conn, conn->close_reason);
                if (conn->retry_count == conn->config.close_retries) {
                    kcpmux_conn_close_internal(conn, conn->close_reason);
                    return;
                }
            } else {
                kcpmux_conn_close_internal(conn, conn->close_reason);
                return;
            }
        }
        break;

    default:
        break;
    }

    kcpmux_conn_refresh_timer(conn, now);
}

void kcpmux_conn_refresh_timer(kcpmux_conn_t *conn, int64_t now)
{
    int64_t deadline_ms;

    if (!conn || !conn->engine || conn->internal_closed) {
        return;
    }
    if (!kcpmux_conn_calculate_deadline(conn, &deadline_ms)) {
        kcpmux_engine_cancel_timer_node(conn->engine, &conn->timer_node, now);
        return;
    }
    (void)kcpmux_engine_schedule_timer_node(conn->engine, &conn->timer_node, deadline_ms, now);
}

void kcpmux_conn_note_receive(kcpmux_conn_t *conn, int64_t recv_time_ms, uint8_t is_payload)
{
    if (!conn || conn->internal_closed) {
        return;
    }
    if (recv_time_ms > conn->last_recv_ts) {
        conn->last_recv_ts = recv_time_ms;
    }
    if (is_payload && recv_time_ms > conn->last_payload_ts) {
        conn->last_payload_ts = recv_time_ms;
    }
    kcpmux_conn_refresh_timer(conn, recv_time_ms);
}

static void kcpmux_conn_timer_callback(kcpmux_timer_node_t *node, int64_t now_ms)
{
    kcpmux_conn_t *conn = node ? (kcpmux_conn_t *)node->owner : NULL;
    kcpmux_conn_update(conn, now_ms);
}

// ============================================================================
// Stream management
// ============================================================================

void kcpmux_conn_add_stream(kcpmux_conn_t *conn, kcpmux_stream_t *stream)
{
    if (!conn || !stream || conn->internal_closed || stream->internal_closed ||
        stream->in_stream_map) return;

    uint32_t hash = kcpmux_hash32(&stream->stream_id, sizeof(stream->stream_id));
    kcpmux_htb_add(conn->stream_map, &stream->hash_node, &stream->stream_id, hash);
    conn->stream_count++;
    stream->in_stream_map = 1;

    // Update engine stats
    if (conn->engine) {
        conn->engine->stats.stream_count++;
    }
}

void kcpmux_conn_remove_stream(kcpmux_conn_t *conn, kcpmux_stream_t *stream)
{
    if (!conn || !stream || !stream->in_stream_map) return;

    kcpmux_htb_del(conn->stream_map, &stream->hash_node);
    conn->stream_count--;
    stream->in_stream_map = 0;

    // Update engine stats
    if (conn->engine) {
        conn->engine->stats.stream_count--;
    }
}

kcpmux_stream_t *kcpmux_conn_get_stream_by_id(kcpmux_conn_t *conn, uint32_t stream_id)
{
    if (!conn) return NULL;

    uint32_t hash = kcpmux_hash32(&stream_id, sizeof(stream_id));
    list_head *node = kcpmux_htb_find(conn->stream_map, &stream_id, hash);
    if (!node) return NULL;

    return list_entry(node, kcpmux_stream_t, hash_node);
}

uint32_t kcpmux_conn_alloc_stream_id(kcpmux_conn_t *conn)
{
    uint32_t attempts;

    if (!conn) return 0;
    for (attempts = 0; attempts <= conn->stream_count; attempts++) {
        uint32_t id = conn->next_stream_id;
        if (id == 0) {
            id = conn->is_initiator ? 1U : 2U;
        }
        conn->next_stream_id = id + 2U;
        if (!kcpmux_conn_get_stream_by_id(conn, id)) {
            return id;
        }
    }
    return 0;
}

int kcpmux_conn_is_new_peer_stream_id(const kcpmux_conn_t *conn, uint32_t stream_id)
{
    uint32_t delta;

    if (!conn || stream_id == 0) return 0;
    if ((stream_id & 1U) == conn->is_initiator) return 0;
    if (!conn->peer_stream_id_initialized) return 1;
    delta = stream_id - conn->latest_peer_stream_id;
    return delta != 0 && delta < 0x80000000U;
}

// ============================================================================
// Message send (implemented in kcpmux_protocol.c)
// ============================================================================

int kcpmux_conn_send_connect(kcpmux_conn_t *conn)
{
    if (!conn || conn->internal_closed) return KCPMUX_ERR_CLOSED;
    conn->last_ctrl_ts = kcpmux_engine_now(conn->engine);
    int ret = kcpmux_protocol_send_conn_connect(conn);
    kcpmux_conn_refresh_timer(conn, conn->last_ctrl_ts);
    return ret;
}

int kcpmux_conn_send_connect_ack(kcpmux_conn_t *conn, uint8_t result)
{
    return kcpmux_protocol_send_conn_connect_ack(conn, result);
}

int kcpmux_conn_send_keepalive(kcpmux_conn_t *conn)
{
    if (!conn || conn->internal_closed) return KCPMUX_ERR_CLOSED;
    conn->last_keepalive_ts = kcpmux_engine_now(conn->engine);
    conn->keepalive_seq++;
    int ret = kcpmux_protocol_send_conn_keepalive(conn);
    kcpmux_conn_refresh_timer(conn, conn->last_keepalive_ts);
    return ret;
}

int kcpmux_conn_send_close(kcpmux_conn_t *conn, uint8_t reason)
{
    if (!conn || conn->internal_closed) return KCPMUX_ERR_CLOSED;
    conn->last_ctrl_ts = kcpmux_engine_now(conn->engine);
    int ret = kcpmux_protocol_send_conn_close(conn, reason);
    kcpmux_conn_refresh_timer(conn, conn->last_ctrl_ts);
    return ret;
}

int kcpmux_conn_send_close_ack(kcpmux_conn_t *conn, uint8_t reason)
{
    return kcpmux_protocol_send_conn_close_ack(conn, reason);
}

int kcpmux_conn_write_socket(kcpmux_conn_t *conn, const uint8_t *buf, unsigned size)
{
    kcpmux_engine_t *engine = conn->engine;
    int ret = kcpmux_engine_write_socket(engine, buf, size, &conn->peer_addr);
    if (ret == 0) {
        conn->stats.tx_packets++;
        conn->stats.tx_bytes += size;
    }
    return ret;
}
