#include "kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_stream.h"
#include "kcpmux_protocol.h"

#include <stdlib.h>
#include <string.h>

static void kcpmux_stream_timer_callback(kcpmux_timer_node_t *node, int64_t now_ms);
static void kcpmux_stream_advance_local_close(kcpmux_stream_t *stream, int64_t now);
static void kcpmux_stream_clear_pending(kcpmux_stream_t *stream);

static int64_t kcpmux_stream_earlier_deadline(int64_t first, int64_t second)
{
    return first < second ? first : second;
}

static int kcpmux_stream_kcp_active(const kcpmux_stream_t *stream)
{
    return stream->state == KCPMUX_STREAM_STATE_OPEN ||
        (stream->state == KCPMUX_STREAM_STATE_CLOSING &&
         stream->close_phase == KCPMUX_STREAM_CLOSE_LOCAL_DRAIN);
}

static void kcpmux_stream_schedule_finalize(
    kcpmux_stream_t *stream,
    uint8_t reason,
    int64_t now)
{
    stream->close_reason = reason;
    stream->close_phase = KCPMUX_STREAM_CLOSE_FINALIZE;
    kcpmux_stream_clear_pending(stream);
    (void)kcpmux_engine_schedule_timer_node(
        stream->conn->engine, &stream->timer_node, now, now);
}

static void kcpmux_stream_clear_pending(kcpmux_stream_t *stream)
{
    stream->pending_count = 0;
    if (!list_empty(&stream->pending_batch_node)) {
        list_del_init(&stream->pending_batch_node);
    }
}

static void kcpmux_stream_accumulate_update(kcpmux_stream_t *stream, int64_t now_ms)
{
    uint32_t threshold = stream->config.batch_threshold;

    if (threshold <= 1) {
        (void)kcpmux_engine_schedule_timer_node(
            stream->conn->engine, &stream->timer_node, now_ms, now_ms);
        return;
    }
    if (stream->pending_count >= threshold) {
        return;
    }
    if (stream->pending_count == 0) {
        list_add_tail(
            &stream->pending_batch_node,
            &stream->conn->engine->pending_batch_streams);
    }
    stream->pending_count++;
    if (stream->pending_count == threshold) {
        list_del_init(&stream->pending_batch_node);
        (void)kcpmux_engine_schedule_timer_node(
            stream->conn->engine, &stream->timer_node, now_ms, now_ms);
    }
}

static void kcpmux_stream_free(kcpmux_pending_release_t *item)
{
    kcpmux_stream_t *stream = list_entry(item, kcpmux_stream_t, pending_release);
    free(stream);
}

// ============================================================================
// KCP callback
// ============================================================================

// KCP output callback
// Returns: 0 on success, -1 on error
static int __kcp_output(const char *buf, int len, void *kcp, void *user)
{
    kcpmux_stream_t *stream = (kcpmux_stream_t *)user;
    if (!stream || !stream->conn) return -1;
    if (stream->internal_closed || !kcpmux_stream_kcp_active(stream) ||
        stream->conn->internal_closed ||
        stream->conn->state != KCPMUX_CONN_STATE_CONNECTED) {
        return -1;
    }

    (void)kcp;

    // Add message header and send
    int ret = kcpmux_protocol_send_stream_payload(stream, (const uint8_t *)buf, len);
    if (ret == 0) {
        stream->stats.tx_packets++;
        stream->stats.tx_bytes += len;
    }
    return ret == 0 ? 0 : -1;
}

// ============================================================================
// Stream lifecycle
// ============================================================================

kcpmux_stream_t *kcpmux_stream_new(
    kcpmux_conn_t *conn,
    uint32_t stream_id,
    const kcpmux_stream_config_t *config,
    uint8_t is_initiator)
{
    if (!conn || stream_id == 0) return NULL;

    kcpmux_stream_t *stream = (kcpmux_stream_t *)malloc(sizeof(kcpmux_stream_t));
    if (!stream) return NULL;

    memset(stream, 0, sizeof(*stream));
    INIT_LIST_HEAD(&stream->hash_node);
    INIT_LIST_HEAD(&stream->pending_release.node);
    INIT_LIST_HEAD(&stream->pending_batch_node);

    stream->stream_id = stream_id;
    stream->is_initiator = is_initiator;
    stream->conn = conn;

    // Config
    const kcpmux_stream_config_t *source = config
        ? config
        : &conn->engine->default_stream_config;
    if (!kcpmux_stream_config_prepare(&stream->config, source)) {
        free(stream);
        return NULL;
    }

    // Create KCP instance
    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    stream->kcp = ops->create(stream->stream_id, stream, stream->conn->engine->user_data);
    if (!stream->kcp) {
        free(stream);
        return NULL;
    }

    ops->setmss(stream->kcp, stream->config.kcp_mss);
    ops->setoutput(stream->kcp, __kcp_output);

    if (kcpmux_engine_register_timer_node(
            conn->engine,
            &stream->timer_node,
            stream,
            kcpmux_stream_timer_callback) != KCPMUX_ERR_OK) {
        ops->release(stream->kcp);
        free(stream);
        return NULL;
    }

    stream->read_block_start_ts = kcpmux_engine_now(stream->conn->engine);
    stream->read_blocked = 1;
    stream->stats.read_block_count = 1; // TTFB
    stream->conn->engine->stats.stream_opened_total++;
    stream->conn->engine->stats.stream_created_total++;

    stream->state = KCPMUX_STREAM_STATE_OPEN;
    kcpmux_stream_refresh_timer(stream, stream->read_block_start_ts);
    return stream;
}

void kcpmux_stream_close_internal(kcpmux_stream_t *stream, uint8_t reason)
{
    if (!stream) return;

    // Skip if already closed
    if (stream->internal_closed) {
        return;
    } else {
        stream->internal_closed = 1;
    }

    kcpmux_engine_t *engine = stream->conn->engine;
    kcpmux_stream_clear_pending(stream);
    kcpmux_engine_unregister_timer_node(engine, &stream->timer_node);
    kcpmux_conn_remove_stream(stream->conn, stream);

    // Update statistics
    engine->stats.stream_closed_total++;

    // A custom KCP implementation may retain engine_user or other external
    // context. Release it before close notifications, which may release the
    // corresponding external wrapper.
    if (stream->kcp) {
        engine->kcp_ops->release(stream->kcp);
        stream->kcp = NULL;
    }

    // Set state before callback (so callback can query final state)
    if (stream->state != KCPMUX_STREAM_STATE_ERROR) {
        stream->state = KCPMUX_STREAM_STATE_CLOSED;
    }

    // Trigger close callback
    if (stream->callbacks.stream_close_notify) {
        stream->callbacks.stream_close_notify(stream, reason, stream->user_data);
    }

    // The terminal handle is invalid after the notification returns. Defer its
    // physical free until the enclosing engine operation completes.
    kcpmux_engine_queue_release(engine, &stream->pending_release, kcpmux_stream_free);
}

// ============================================================================
// Stream public API
// ============================================================================

kcpmux_stream_t *kcpmux_stream_create(
    kcpmux_conn_t *conn,
    const kcpmux_stream_config_t *config,
    const kcpmux_stream_callbacks_t *callbacks,
    void *user_data)
{
    if (!conn || conn->internal_closed) return NULL;

    kcpmux_engine_t *engine = conn->engine;
    kcpmux_engine_operation_enter(engine);

    // Update API call statistics
    conn->engine->stats.api_stream_create_calls++;

    // Check connection state
    if (conn->state != KCPMUX_CONN_STATE_CONNECTED) {
        kcpmux_engine_operation_leave(engine);
        return NULL;
    }

    // Allocate stream_id
    uint32_t stream_id = kcpmux_conn_alloc_stream_id(conn);
    if (stream_id == 0) {
        kcpmux_engine_operation_leave(engine);
        return NULL;
    }

    // Create stream
    kcpmux_stream_t *stream = kcpmux_stream_new(conn, stream_id, config, 1);
    if (!stream) {
        kcpmux_engine_operation_leave(engine);
        return NULL;
    }

    // Set callbacks
    if (callbacks) {
        stream->callbacks = *callbacks;
    }
    stream->user_data = user_data;

    // Add to connection
    kcpmux_conn_add_stream(conn, stream);

    kcpmux_engine_operation_leave(engine);
    return stream;
}

int kcpmux_stream_close(kcpmux_stream_t *stream)
{
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_engine_t *engine = stream->conn->engine;
    kcpmux_engine_operation_enter(engine);

    // Update API call statistics
    stream->conn->engine->stats.api_stream_close_calls++;

    if (stream->state == KCPMUX_STREAM_STATE_CLOSING) {
        // A local close while passively draining means the application no
        // longer wants the unread tail. Complete outside this API stack.
        if (stream->close_phase == KCPMUX_STREAM_CLOSE_REMOTE_DRAIN) {
            kcpmux_stream_schedule_finalize(
                stream, KCPMUX_CLOSE_REASON_NORMAL,
                kcpmux_engine_now(stream->conn->engine));
        }
        kcpmux_engine_operation_leave(engine);
        return 0;
    }
    if (stream->state == KCPMUX_STREAM_STATE_CLOSED ||
        stream->state == KCPMUX_STREAM_STATE_ERROR) {
        kcpmux_engine_operation_leave(engine);
        return 0;
    }

    // Initialize all closing anchors before exposing the state transition.
    int64_t now = kcpmux_engine_now(stream->conn->engine);
    stream->last_ctrl_ts = now;
    stream->drain_started_ts = now;
    stream->close_reason = KCPMUX_CLOSE_REASON_NORMAL;
    stream->retry_count = 0;
    stream->close_phase = KCPMUX_STREAM_CLOSE_LOCAL_DRAIN;
    kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_CLOSING);
    // For locally initiated streams that never sent payload, close directly
    // to avoid sending STREAM_CLOSE for a stream unknown to peer.
    if (stream->is_initiator && stream->stats.up_sent_bytes == 0) {
        kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);
        kcpmux_engine_operation_leave(engine);
        return 0;
    }

    // Flush batched sends and begin transport draining immediately.
    (void)kcpmux_engine_schedule_timer_node(
        engine, &stream->timer_node, now, now);
    kcpmux_stream_advance_local_close(stream, now);

    kcpmux_engine_operation_leave(engine);
    return 0;
}

void kcpmux_stream_set_config(kcpmux_stream_t *stream, const kcpmux_stream_config_t *config)
{
    if (!stream || !config || stream->internal_closed) return;

    kcpmux_stream_config_t prepared;
    if (!kcpmux_stream_config_prepare(&prepared, config)) return;
    prepared.kcp_mss = stream->config.kcp_mss;
    stream->config = prepared;
    kcpmux_stream_clear_pending(stream);
    kcpmux_stream_refresh_timer(stream, kcpmux_engine_now(stream->conn->engine));
}

void kcpmux_stream_set_callbacks(
    kcpmux_stream_t *stream,
    const kcpmux_stream_callbacks_t *callbacks,
    void *user_data)
{
    if (!stream || stream->internal_closed) return;

    if (callbacks) {
        stream->callbacks = *callbacks;
    } else {
        memset(&stream->callbacks, 0, sizeof(stream->callbacks));
    }
    stream->user_data = user_data;
}

int kcpmux_stream_send(kcpmux_stream_t *stream, const uint8_t *buf, unsigned size, int flush)
{
    if (!stream || !buf || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_engine_t *engine = stream->conn->engine;
    int64_t now = kcpmux_engine_now(engine);
    kcpmux_engine_operation_enter(engine);

    // Update API call statistics
    stream->conn->engine->stats.api_stream_send_calls++;

    if (stream->internal_closed) {
        kcpmux_engine_operation_leave(engine);
        return KCPMUX_ERR_CLOSED;
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN ||
        stream->conn->state != KCPMUX_CONN_STATE_CONNECTED) {
        kcpmux_engine_operation_leave(engine);
        return KCPMUX_ERR_STATE;
    }

    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    // Check if send buffer is becoming full
    int waitsnd = ops->waitsnd(stream->kcp);
    if (waitsnd >= (int)stream->config.send_pause_threshold) {
        // Record write block start time if this is a new block
        if (!stream->write_blocked) {
            stream->write_block_start_ts = now;
            stream->write_blocked = 1;
            stream->stats.write_block_count++;
        }
        kcpmux_engine_operation_leave(engine);
        return 0;  // Buffer full
    }

    ops->current_update(stream->kcp, now);

    // Split data into chunks based on kcp_mss and send each chunk
    unsigned chunk_size = stream->config.kcp_mss;
    unsigned offset = 0;
    unsigned remaining = size;
    unsigned total_sent = 0;

    while (remaining > 0) {
        unsigned send_size = (remaining < chunk_size) ? remaining : chunk_size;
        int ret = ops->send(stream->kcp, (const char *)(buf + offset), (int)send_size);
        if (ret < 0) {
            if (total_sent == 0) {
                kcpmux_engine_operation_leave(engine);
                return KCPMUX_ERR_KCPRET(ret);
            }
            break;
        }

        total_sent += send_size;
        offset += send_size;
        remaining -= send_size;
    }

    // Update statistics
    stream->stats.up_sent_bytes += total_sent;

    if (total_sent == 0) {
        kcpmux_engine_operation_leave(engine);
        return 0;
    }

    // A send mutates opaque KCP timing. Batched sends keep the current KCP
    // deadline until the threshold or finish boundary requests an update;
    // synchronous flush can trust check() after update().
    if (flush) {
        kcpmux_stream_clear_pending(stream);
        ops->update(stream->kcp, now);
        int64_t deadline_ms = ops->check(stream->kcp, now);
        (void)kcpmux_engine_schedule_timer_node(engine, &stream->timer_node, deadline_ms, now);
    } else {
        kcpmux_stream_accumulate_update(stream, now);
    }

    kcpmux_engine_operation_leave(engine);
    return (int)total_sent;
}

void kcpmux_stream_finish_batch(kcpmux_stream_t *stream)
{
    if (!stream) return;
    kcpmux_stream_finish_batch_at(stream, kcpmux_engine_now(stream->conn->engine));
}

void kcpmux_stream_finish_batch_at(kcpmux_stream_t *stream, int64_t now)
{
    if (!stream || stream->internal_closed || stream->pending_count == 0) {
        return;
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN ||
        stream->conn->state != KCPMUX_CONN_STATE_CONNECTED) {
        return;
    }
    if (stream->config.batch_threshold > 1 &&
        stream->pending_count >= stream->config.batch_threshold) {
        return;
    }
    if (!list_empty(&stream->pending_batch_node)) {
        list_del_init(&stream->pending_batch_node);
    }
    stream->pending_count = stream->config.batch_threshold > 1
        ? stream->config.batch_threshold
        : 0;
    (void)kcpmux_engine_schedule_timer_node(
        stream->conn->engine, &stream->timer_node, now, now);
}

int kcpmux_stream_peek_size(kcpmux_stream_t *stream)
{
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;
    if (stream->internal_closed) {
        return KCPMUX_ERR_CLOSED;
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN &&
        !(stream->state == KCPMUX_STREAM_STATE_CLOSING &&
          stream->close_phase == KCPMUX_STREAM_CLOSE_REMOTE_DRAIN)) {
        return KCPMUX_ERR_STATE;
    }

    int ret = stream->conn->engine->kcp_ops->peeksize(stream->kcp);
    return ret > 0 ? ret : 0;
}

int kcpmux_stream_recv(kcpmux_stream_t *stream, uint8_t *buf, unsigned size)
{
    if (!stream || !buf || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    // Update API call statistics
    stream->conn->engine->stats.api_stream_recv_calls++;

    int peek_size = kcpmux_stream_peek_size(stream);
    if (peek_size < 0) return peek_size;
    if ((unsigned)peek_size > size) return KCPMUX_ERR_BUFFER_TOO_SMALL;
    if (peek_size == 0) {
        // Record read block start time if this is a new block
        if (!stream->read_blocked) {
            stream->read_block_start_ts = kcpmux_engine_now(stream->conn->engine);
            stream->read_blocked = 1;
            stream->stats.read_block_count++;
        }
        return 0;
    }

    // Receive data from KCP
    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;
    int ret = ops->recv(stream->kcp, (char *)buf, peek_size);
    if (ret < 0) {
        return KCPMUX_ERR_KCPRET(ret);
    }

    // Update statistics
    stream->stats.up_recv_bytes += ret;

    if (stream->state == KCPMUX_STREAM_STATE_CLOSING &&
        stream->close_phase == KCPMUX_STREAM_CLOSE_REMOTE_DRAIN &&
        ops->peeksize(stream->kcp) <= 0) {
        kcpmux_stream_schedule_finalize(
            stream, stream->close_reason,
            kcpmux_engine_now(stream->conn->engine));
    }

    return ret;
}

// ============================================================================
// Stream query API
// ============================================================================

uint32_t kcpmux_stream_id(kcpmux_stream_t *stream)
{
    return stream ? stream->stream_id : 0;
}

uint8_t kcpmux_stream_get_state(kcpmux_stream_t *stream)
{
    return stream ? stream->state : KCPMUX_STREAM_STATE_ERROR;
}

void *kcpmux_stream_get_kcp(kcpmux_stream_t *stream)
{
    return stream ? stream->kcp : NULL;
}

kcpmux_conn_t *kcpmux_stream_get_conn(kcpmux_stream_t *stream)
{
    return stream ? stream->conn : NULL;
}

void *kcpmux_stream_get_user_data(kcpmux_stream_t *stream)
{
    return stream ? stream->user_data : NULL;
}

void kcpmux_stream_get_stats(kcpmux_stream_t *stream, kcpmux_stream_stats_t *stats)
{
    if (!stats) return;

    if (!stream) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    memcpy(stats, &stream->stats, sizeof(*stats));
}

// ============================================================================
// Stream internal functions
// ============================================================================

void kcpmux_stream_set_state(kcpmux_stream_t *stream, uint8_t new_state)
{
    if (!stream || stream->state == new_state) return;

    uint8_t old_state = stream->state;
    stream->state = new_state;

    kcpmux_stream_refresh_timer(stream, kcpmux_engine_now(stream->conn->engine));

    if (stream->callbacks.stream_state_changed) {
        stream->callbacks.stream_state_changed(stream, old_state, new_state, stream->user_data);
    }
}

static void kcpmux_stream_advance_local_close(kcpmux_stream_t *stream, int64_t now)
{
    if (!stream || stream->internal_closed ||
        stream->close_phase != KCPMUX_STREAM_CLOSE_LOCAL_DRAIN ||
        stream->conn->engine->kcp_ops->waitsnd(stream->kcp) != 0) {
        return;
    }

    stream->close_phase = KCPMUX_STREAM_CLOSE_WAIT_ACK;
    stream->retry_count = 0;
    kcpmux_stream_send_close(stream, KCPMUX_CLOSE_REASON_NORMAL);
    if (stream->config.close_retries == 0) {
        kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);
    }
}

void kcpmux_stream_update(kcpmux_stream_t *stream, int64_t now)
{
    if (!stream || stream->internal_closed || stream->conn->internal_closed ||
        stream->conn->state != KCPMUX_CONN_STATE_CONNECTED) return;

    switch (stream->state) {
    case KCPMUX_STREAM_STATE_OPEN:
        {
            kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

            // Update KCP
            kcpmux_stream_clear_pending(stream);
            ops->update(stream->kcp, now);

            // Check flow control recovery (edge-triggered: blocked -> writable)
            if (stream->write_blocked) {
                int waitsnd = ops->waitsnd(stream->kcp);
                if (waitsnd < (int)stream->config.send_resume_threshold) {
                    // Calculate and record write block time
                    if (stream->write_block_start_ts > 0) {
                        stream->stats.write_block_time_ms += (now - stream->write_block_start_ts);
                        stream->write_block_start_ts = 0;
                    }
                    stream->write_blocked = 0;
                    if (stream->callbacks.stream_write_notify) {
                        stream->callbacks.stream_write_notify(stream, stream->user_data);
                    }
                }
            }

            int64_t deadline_ms = ops->check(stream->kcp, now);
            (void)kcpmux_engine_schedule_timer_node(
                stream->conn->engine,
                &stream->timer_node,
                deadline_ms,
                now);
        }
        break;

    case KCPMUX_STREAM_STATE_CLOSING:
        switch (stream->close_phase) {
        case KCPMUX_STREAM_CLOSE_LOCAL_DRAIN: {
            kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;
            kcpmux_stream_clear_pending(stream);
            ops->update(stream->kcp, now);
            if (ops->waitsnd(stream->kcp) == 0) {
                kcpmux_stream_advance_local_close(stream, now);
                return;
            }
            if (now >= kcpmux_timer_deadline_after(
                    stream->drain_started_ts, stream->config.drain_timeout_ms)) {
                stream->close_reason = KCPMUX_CLOSE_REASON_TIMEOUT;
                kcpmux_stream_send_close(stream, KCPMUX_CLOSE_REASON_TIMEOUT);
                kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_TIMEOUT);
                return;
            }
            kcpmux_stream_refresh_timer(stream, now);
            break;
        }

        case KCPMUX_STREAM_CLOSE_WAIT_ACK:
            if (now >= kcpmux_timer_deadline_after(
                    stream->last_ctrl_ts, stream->config.ctrl_timeout_ms)) {
                if (stream->retry_count < stream->config.close_retries) {
                    stream->retry_count++;
                    kcpmux_stream_send_close(stream, stream->close_reason);
                    if (stream->retry_count == stream->config.close_retries) {
                        kcpmux_stream_close_internal(stream, stream->close_reason);
                        return;
                    }
                } else {
                    kcpmux_stream_close_internal(stream, stream->close_reason);
                    return;
                }
            }
            break;

        case KCPMUX_STREAM_CLOSE_REMOTE_DRAIN:
            if (now >= kcpmux_timer_deadline_after(
                    stream->drain_started_ts, stream->config.drain_timeout_ms)) {
                kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_TIMEOUT);
                return;
            }
            break;

        case KCPMUX_STREAM_CLOSE_FINALIZE:
            kcpmux_stream_close_internal(stream, stream->close_reason);
            return;

        default:
            break;
        }
        break;

    default:
        kcpmux_engine_cancel_timer_node(stream->conn->engine, &stream->timer_node, now);
        break;
    }
}

void kcpmux_stream_refresh_timer(kcpmux_stream_t *stream, int64_t now)
{
    int64_t deadline_ms;

    if (!stream || !stream->conn ||
        !stream->timer_node.owner) {
        return;
    }
    if (stream->internal_closed) {
        kcpmux_engine_cancel_timer_node(stream->conn->engine, &stream->timer_node, now);
        return;
    }
    if (stream->conn->internal_closed ||
        stream->conn->state != KCPMUX_CONN_STATE_CONNECTED) {
        kcpmux_engine_cancel_timer_node(stream->conn->engine, &stream->timer_node, now);
        return;
    }
    switch (stream->state) {
    case KCPMUX_STREAM_STATE_OPEN:
        deadline_ms = stream->conn->engine->kcp_ops->check(stream->kcp, now);
        break;

    case KCPMUX_STREAM_STATE_CLOSING:
        switch (stream->close_phase) {
        case KCPMUX_STREAM_CLOSE_LOCAL_DRAIN:
            deadline_ms = kcpmux_stream_earlier_deadline(
                stream->conn->engine->kcp_ops->check(stream->kcp, now),
                kcpmux_timer_deadline_after(
                    stream->drain_started_ts, stream->config.drain_timeout_ms));
            break;
        case KCPMUX_STREAM_CLOSE_WAIT_ACK:
            deadline_ms = kcpmux_timer_deadline_after(
                stream->last_ctrl_ts, stream->config.ctrl_timeout_ms);
            break;
        case KCPMUX_STREAM_CLOSE_REMOTE_DRAIN:
            deadline_ms = kcpmux_timer_deadline_after(
                stream->drain_started_ts, stream->config.drain_timeout_ms);
            break;
        case KCPMUX_STREAM_CLOSE_FINALIZE:
            deadline_ms = now;
            break;
        default:
            kcpmux_engine_cancel_timer_node(stream->conn->engine, &stream->timer_node, now);
            return;
        }
        break;

    default:
        kcpmux_engine_cancel_timer_node(stream->conn->engine, &stream->timer_node, now);
        return;
    }
    (void)kcpmux_engine_schedule_timer_node(
        stream->conn->engine,
        &stream->timer_node,
        deadline_ms,
        now);
}

static void kcpmux_stream_timer_callback(kcpmux_timer_node_t *node, int64_t now_ms)
{
    kcpmux_stream_t *stream = node ? (kcpmux_stream_t *)node->owner : NULL;
    kcpmux_stream_update(stream, now_ms);
}

// ============================================================================
// Message handling
// ============================================================================

int kcpmux_stream_handle_payload(
    kcpmux_stream_t *stream,
    const uint8_t *buf,
    unsigned size,
    int64_t recv_time_ms)
{
    if (!stream || !buf || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    if (stream->internal_closed) {
        return KCPMUX_ERR_CLOSED;
    }
    if (!kcpmux_stream_kcp_active(stream)) {
        return KCPMUX_ERR_STATE;
    }

    // Update statistics
    stream->stats.rx_packets++;
    stream->stats.rx_bytes += size;

    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    // Input to KCP
    ops->current_update(stream->kcp, recv_time_ms);
    int ret = ops->input(stream->kcp, (const char *)buf, (long)size);
    // input() may partially mutate a custom KCP implementation even on error.
    kcpmux_stream_accumulate_update(stream, recv_time_ms);
    if (ret < 0) {
        return KCPMUX_ERR_KCPRET(ret);
    }

    // Check flow control recovery (edge-triggered: blocked -> readable)
    if (stream->state == KCPMUX_STREAM_STATE_OPEN && stream->read_blocked) {
        int peeksize = ops->peeksize(stream->kcp);
        if (peeksize > 0) {
            // Calculate and record read block time
            if (stream->read_block_start_ts > 0) {
                if (stream->stats.read_block_count == 1) {
                    stream->stats.ttfb_time_ms = (recv_time_ms - stream->read_block_start_ts);
                }
                stream->stats.read_block_time_ms += (recv_time_ms - stream->read_block_start_ts);
                stream->read_block_start_ts = 0;
            }
            stream->read_blocked = 0;
            if (stream->callbacks.stream_read_notify) {
                stream->callbacks.stream_read_notify(stream, stream->user_data);
            }
        }
    }

    return 0;
}

int kcpmux_stream_handle_close(kcpmux_stream_t *stream, uint8_t reason)
{
    if (!stream || stream->internal_closed) return KCPMUX_ERR_CLOSED;

    if (reason != KCPMUX_CLOSE_REASON_NORMAL) {
        kcpmux_stream_send_close_ack(stream, reason);
        kcpmux_stream_close_internal(stream, reason);
        return 0;
    }

    int64_t now = kcpmux_engine_now(stream->conn->engine);
    if (stream->state == KCPMUX_STREAM_STATE_CLOSING) {
        switch (stream->close_phase) {
        case KCPMUX_STREAM_CLOSE_LOCAL_DRAIN:
            // A peer close wins a simultaneous close. Acknowledge it now and
            // abandon locally queued KCP data instead of allowing both sides
            // to wait for each other's close lifecycle.
            kcpmux_stream_send_close_ack(stream, reason);
            kcpmux_stream_schedule_finalize(stream, KCPMUX_CLOSE_REASON_NORMAL, now);
            return 0;
        case KCPMUX_STREAM_CLOSE_WAIT_ACK:
            kcpmux_stream_send_close_ack(stream, reason);
            kcpmux_stream_schedule_finalize(stream, KCPMUX_CLOSE_REASON_NORMAL, now);
            return 0;
        case KCPMUX_STREAM_CLOSE_REMOTE_DRAIN:
        case KCPMUX_STREAM_CLOSE_FINALIZE:
            kcpmux_stream_send_close_ack(stream, reason);
            return 0;
        default:
            return KCPMUX_ERR_STATE;
        }
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN) return KCPMUX_ERR_STATE;

    kcpmux_stream_send_close_ack(stream, reason);
    stream->close_reason = reason;
    stream->close_phase = KCPMUX_STREAM_CLOSE_REMOTE_DRAIN;
    stream->drain_started_ts = now;
    kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_CLOSING);
    if (stream->conn->engine->kcp_ops->peeksize(stream->kcp) <= 0) {
        kcpmux_stream_schedule_finalize(stream, reason, now);
    }
    return 0;
}

// ============================================================================
// Message send (implemented in kcpmux_protocol.c)
// ============================================================================

int kcpmux_stream_send_close(kcpmux_stream_t *stream, uint8_t reason)
{
    if (!stream || stream->internal_closed) return KCPMUX_ERR_CLOSED;
    int64_t now = kcpmux_engine_now(stream->conn->engine);
    stream->last_ctrl_ts = now;
    int ret = kcpmux_protocol_send_stream_close(stream, reason);
    kcpmux_stream_refresh_timer(stream, now);
    return ret;
}

int kcpmux_stream_send_close_ack(kcpmux_stream_t *stream, uint8_t reason)
{
    return kcpmux_protocol_send_stream_close_ack(stream, reason);
}
