#include "kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_stream.h"
#include "kcpmux_protocol.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// KCP callback
// ============================================================================

// KCP output callback
// Returns: 0 on success, -1 on error
static int __kcp_output(const char *buf, int len, void *kcp, void *user) {
    kcpmux_stream_t *stream = (kcpmux_stream_t *)user;
    if (!stream || !stream->conn) return -1;

    (void)kcp;

    // Add message header and send
    int ret = kcpmux_protocol_send_stream_payload(stream, (const uint8_t *)buf, len);
    if (ret == 0) {
        stream->stats.tx_packets++;
        stream->stats.tx_bytes += len;
        return 0;
    }
    return -1;
}

// ============================================================================
// Stream lifecycle
// ============================================================================

kcpmux_stream_t *kcpmux_stream_new(kcpmux_conn_t *conn,
                                 uint32_t stream_id,
                                 const kcpmux_stream_config_t *config,
                                 uint8_t is_initiator)
{
    if (!conn || stream_id == 0) return NULL;

    kcpmux_stream_t *stream = (kcpmux_stream_t *)malloc(sizeof(kcpmux_stream_t));
    if (!stream) return NULL;

    memset(stream, 0, sizeof(*stream));
    INIT_LIST_HEAD(&stream->hash_node);

    stream->stream_id = stream_id;
    stream->is_initiator = is_initiator;
    stream->conn = conn;

    // Config
    if (config) {
        stream->config = *config;
    } else {
        // Use engine's default stream config
        kcpmux_engine_t *engine = conn->engine;
        stream->config = engine->default_stream_config;
    }

    stream->conn->engine->stats.stream_created_total++;

    // Create KCP instance
    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    stream->kcp = ops->create(stream->stream_id, stream, stream->conn->engine->user_data);
    if (!stream->kcp) {
        free(stream);
        return NULL;
    }

    ops->setmss(stream->kcp, stream->config.kcp_mss);
    ops->setoutput(stream->kcp, __kcp_output);

    stream->read_block_start_ts = kcpmux_engine_now(stream->conn->engine);
    stream->read_blocked = 1;
    stream->stats.read_block_count = 1; // TTFB
    stream->conn->engine->stats.stream_opened_total++;

    stream->state = KCPMUX_STREAM_STATE_OPEN;
    return stream;
}

void kcpmux_stream_close_internal(kcpmux_stream_t *stream, uint8_t reason) {
    if (!stream) return;

    // Skip if already closed
    if (stream->internal_closed) {
        return;
    } else {
        stream->internal_closed = 1;
    }

    // Update statistics
    stream->conn->engine->stats.stream_closed_total++;

    // Destroy KCP
    if (stream->kcp) {
        kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;
        ops->release(stream->kcp);
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
}

void kcpmux_stream_free(kcpmux_stream_t *stream) {
    if (!stream) return;

    // Auto close if not closed
    kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);

    // Remove from connection
    kcpmux_conn_remove_stream(stream->conn, stream);

    free(stream);
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

    // Update API call statistics
    conn->engine->stats.api_stream_create_calls++;

    // Check connection state
    if (conn->state != KCPMUX_CONN_STATE_CONNECTED) {
        return NULL;
    }

    // Allocate stream_id
    uint32_t stream_id = kcpmux_conn_alloc_stream_id(conn);
    if (stream_id == 0) return NULL;

    // Create stream
    kcpmux_stream_t *stream = kcpmux_stream_new(conn, stream_id, config, 1);
    if (!stream) return NULL;

    // Set callbacks
    if (callbacks) {
        stream->callbacks = *callbacks;
    }
    stream->user_data = user_data;

    // Add to connection
    kcpmux_conn_add_stream(conn, stream);

    return stream;
}

int kcpmux_stream_close(kcpmux_stream_t *stream) {
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;

    // Update API call statistics
    stream->conn->engine->stats.api_stream_close_calls++;

    if (stream->state == KCPMUX_STREAM_STATE_CLOSING ||
        stream->state == KCPMUX_STREAM_STATE_CLOSED ||
        stream->state == KCPMUX_STREAM_STATE_ERROR) {
        return 0;
    }

    // Enter closing state
    kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_CLOSING);
    stream->close_ts = kcpmux_engine_now(stream->conn->engine);
    stream->close_reason = KCPMUX_CLOSE_REASON_NORMAL;
    stream->retry_count = 0;

    // For locally initiated streams that never sent payload, close directly
    // to avoid sending STREAM_CLOSE for a stream unknown to peer.
    if (stream->is_initiator && stream->stats.up_sent_bytes == 0) {
        kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);
        return 0;
    }

    // Send close message
    kcpmux_stream_send_close(stream, KCPMUX_CLOSE_REASON_NORMAL);

    return 0;
}

void kcpmux_stream_set_config(kcpmux_stream_t *stream,
                             const kcpmux_stream_config_t *config)
{
    if (!stream || !config || stream->internal_closed) return;

    // Directly replace the entire config structure
    stream->config = *config;
}

void kcpmux_stream_set_callbacks(kcpmux_stream_t *stream,
                                const kcpmux_stream_callbacks_t *callbacks,
                                void *user_data)
{
    if (!stream || stream->internal_closed) return;

    if (callbacks) {
        stream->callbacks = *callbacks;
    }
    stream->user_data = user_data;
}

int kcpmux_stream_send(kcpmux_stream_t *stream,
                      const uint8_t *buf, unsigned size,
                      int flush)
{
    if (!stream || !buf || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    int64_t now = kcpmux_engine_now(stream->conn->engine);

    // Update API call statistics
    stream->conn->engine->stats.api_stream_send_calls++;

    if (stream->internal_closed) {
        return KCPMUX_ERR_CLOSED;
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN) {
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

    // Flush KCP if requested
    if (flush) {
        ops->update(stream->kcp, now);
        stream->next_update_ts = ops->check(stream->kcp, now);
    }

    return (int)total_sent;
}

int kcpmux_stream_recv(kcpmux_stream_t *stream,
                      uint8_t *buf, unsigned size)
{
    if (!stream || !buf || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    // Update API call statistics
    stream->conn->engine->stats.api_stream_recv_calls++;

    if (stream->internal_closed) {
        return KCPMUX_ERR_CLOSED;
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN) {
        return KCPMUX_ERR_STATE;
    }

    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    // Receive data from KCP
    int ret = ops->recv(stream->kcp, (char *)buf, (int)size);
    if (ret < 0) {
        // Record read block start time if this is a new block
        if (!stream->read_blocked) {
            stream->read_block_start_ts = kcpmux_engine_now(stream->conn->engine);
            stream->read_blocked = 1;
            stream->stats.read_block_count++;
        }
        return 0;  // No data to read
    }

    // Update statistics
    stream->stats.up_recv_bytes += ret;

    return ret;
}

// ============================================================================
// Stream query API
// ============================================================================

uint32_t kcpmux_stream_id(kcpmux_stream_t *stream) {
    return stream ? stream->stream_id : 0;
}

uint8_t kcpmux_stream_get_state(kcpmux_stream_t *stream) {
    return stream ? stream->state : KCPMUX_STREAM_STATE_ERROR;
}

void* kcpmux_stream_get_kcp(kcpmux_stream_t *stream) {
    return stream ? stream->kcp : NULL;
}

kcpmux_conn_t *kcpmux_stream_get_conn(kcpmux_stream_t *stream) {
    return stream ? stream->conn : NULL;
}

void *kcpmux_stream_get_user_data(kcpmux_stream_t *stream) {
    return stream ? stream->user_data : NULL;
}

void kcpmux_stream_get_stats(kcpmux_stream_t *stream, kcpmux_stream_stats_t *stats) {
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

void kcpmux_stream_set_state(kcpmux_stream_t *stream, uint8_t new_state) {
    if (!stream || stream->state == new_state) return;

    uint8_t old_state = stream->state;
    stream->state = new_state;

    if (stream->callbacks.stream_state_changed) {
        stream->callbacks.stream_state_changed(stream, old_state, new_state, stream->user_data);
    }
}

void kcpmux_stream_update(kcpmux_stream_t *stream, int64_t now) {
    if (!stream) return;

    // Check if it's time to update
    if (stream->next_update_ts > 0 && now < stream->next_update_ts) {
        return;
    }

    switch (stream->state) {
    case KCPMUX_STREAM_STATE_OPEN:
        {
            kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

            // Update KCP
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
        }
        break;

    case KCPMUX_STREAM_STATE_CLOSING:
        // Check close timeout and retransmit
        if (now - stream->last_ctrl_ts >= stream->config.ctrl_timeout_ms) {
            if (stream->retry_count < stream->config.close_retries) {
                stream->retry_count++;
                kcpmux_stream_send_close(stream, stream->close_reason);
            } else {
                kcpmux_stream_close_internal(stream, stream->close_reason);
                return;
            }
        }
        break;

    default:
        break;
    }
}

int64_t kcpmux_stream_check_interval(kcpmux_stream_t *stream, int64_t now) {
    if (!stream) return 10;

    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    int64_t interval = 10; // Default 10ms

    switch (stream->state) {
    case KCPMUX_STREAM_STATE_OPEN:
        interval = ops->check(stream->kcp, now) - now;
        break;

    case KCPMUX_STREAM_STATE_CLOSING:
        interval = stream->config.ctrl_timeout_ms;
        break;

    default:
        interval = 10;
        break;
    }

    // Set next update timestamp
    stream->next_update_ts = now + interval;

    return interval;
}

// ============================================================================
// Message handling
// ============================================================================

int kcpmux_stream_handle_payload(kcpmux_stream_t *stream, const uint8_t *buf, unsigned size, int64_t recv_time_ms) {
    if (!stream || !buf || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    if (stream->internal_closed) {
        return KCPMUX_ERR_CLOSED;
    }
    if (stream->state != KCPMUX_STREAM_STATE_OPEN) {
        return KCPMUX_ERR_STATE;
    }

    // Update statistics
    stream->stats.rx_packets++;
    stream->stats.rx_bytes += size;

    kcpmux_kcp_ops_t *ops = stream->conn->engine->kcp_ops;

    // Input to KCP
    ops->current_update(stream->kcp, recv_time_ms);
    int ret = ops->input(stream->kcp, (const char *)buf, (long)size);
    if (ret < 0) {
        return KCPMUX_ERR_KCPRET(ret);
    }

    // Check flow control recovery (edge-triggered: blocked -> readable)
    if (stream->read_blocked) {
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

// ============================================================================
// Message send (implemented in kcpmux_protocol.c)
// ============================================================================

int kcpmux_stream_send_close(kcpmux_stream_t *stream, uint8_t reason) {
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;
    stream->last_ctrl_ts = kcpmux_engine_now(stream->conn->engine);
    return kcpmux_protocol_send_stream_close(stream, reason);
}

int kcpmux_stream_send_close_ack(kcpmux_stream_t *stream, uint8_t reason) {
    return kcpmux_protocol_send_stream_close_ack(stream, reason);
}
