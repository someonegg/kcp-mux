#include "kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_stream.h"
#include "kcpmux_protocol.h"

#include <string.h>

// ============================================================================
// Byte order conversion (network byte order)
// ============================================================================

static inline void __write_u16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xff);
}

static inline void __write_u24(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 16);
    buf[1] = (uint8_t)((val >> 8) & 0xff);
    buf[2] = (uint8_t)(val & 0xff);
}

static inline void __write_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)((val >> 16) & 0xff);
    buf[2] = (uint8_t)((val >> 8) & 0xff);
    buf[3] = (uint8_t)(val & 0xff);
}

static inline uint16_t __read_u16(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static inline uint32_t __read_u24(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 16) |
           ((uint32_t)buf[1] << 8) |
           buf[2];
}

static inline uint32_t __read_u32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           buf[3];
}

// ============================================================================
// Protocol input handling
// ============================================================================

int kcpmux_protocol_input(kcpmux_engine_t *engine,
                         const uint8_t *buf, unsigned size,
                         const kcpmux_addr_t *peer_addr,
                         int64_t recv_time_ms)
{
    if (!engine || !buf || size < 1 || !peer_addr || !peer_addr->addr) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t msg_type = buf[0];

    // Find existing connection
    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, peer_addr);

    // Update rx statistics at connection level
    if (conn) {
        conn->stats.rx_packets++;
        conn->stats.rx_bytes += size;
    }

    switch (msg_type) {
    case KCPMUX_MSG_CONN_CONNECT: {
        // Received connection request (passive side)
        if (size < 4) return -KCPMUX_ERR_INVALID_FORMAT;  // Min length: type(1) + version(1) + ext_len(2)

        uint8_t version = buf[1];
        uint16_t ext_len = __read_u16(buf + 2);

        if (ext_len > KCPMUX_PROTO_EXT_MAX_LEN) return -KCPMUX_ERR_INVALID_FORMAT;
        if (size < 4 + ext_len) return -KCPMUX_ERR_INVALID_FORMAT;

        // Version check
        if (version != KCPMUX_VERSION) {
            // Version mismatch, send reject response
            if (conn) {
                kcpmux_conn_send_connect_ack(conn, KCPMUX_ACK_RESULT_VERSION);
            }
            return -KCPMUX_ERR_NOK;
        }

        // If connection exists, might be retransmit, resend ack
        if (conn && conn->state == KCPMUX_CONN_STATE_CONNECTED) {
            kcpmux_conn_send_connect_ack(conn, KCPMUX_ACK_RESULT_OK);
            return 0;
        }

        // Create new connection
        conn = kcpmux_conn_new(engine, peer_addr, NULL, 0);  // is_initiator = 0
        if (!conn) return -KCPMUX_ERR_OOM;

        // Save remote proto_ext
        if (ext_len > 0) {
            memcpy(conn->peer_proto_ext_buf, buf + 4, ext_len);
            conn->peer_proto_ext.data = conn->peer_proto_ext_buf;
            conn->peer_proto_ext.len = ext_len;
        }

        conn->last_recv_ts = recv_time_ms;

        // Add to engine
        kcpmux_engine_add_conn(engine, conn);

        // Notify upper layer
        kcpmux_proto_ext_t resp_ext = {0};
        int result = KCPMUX_ACK_RESULT_ERROR;

        if (engine->callbacks.conn_connect_notify) {
            result = engine->callbacks.conn_connect_notify(
                conn, &conn->peer_proto_ext, &resp_ext, engine->user_data);
        }

        if (result == KCPMUX_ACK_RESULT_OK) {
            // Accept connection
            // Save self response proto_ext
            if (resp_ext.data && resp_ext.len > 0) {
                unsigned len = resp_ext.len;
                if (len > KCPMUX_PROTO_EXT_MAX_LEN) len = KCPMUX_PROTO_EXT_MAX_LEN;
                memcpy(conn->self_proto_ext_buf, resp_ext.data, len);
                conn->self_proto_ext.data = conn->self_proto_ext_buf;
                conn->self_proto_ext.len = len;
            }
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTED);
            kcpmux_conn_send_connect_ack(conn, KCPMUX_ACK_RESULT_OK);
        } else {
            // Reject connection
            // Update statistics
            engine->stats.conn_rejected_total++;
            kcpmux_conn_send_connect_ack(conn, (uint8_t)result);
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_REJECTED);
            kcpmux_conn_free(conn);
        }
        return 0;
    }

    case KCPMUX_MSG_CONN_CONNECT_ACK: {
        // Received connection response (active side)
        if (!conn || conn->state != KCPMUX_CONN_STATE_CONNECTING) return KCPMUX_ERR_STATE;
        if (size < 5) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + version(1) + result(1) + ext_len(2)

        uint8_t result = buf[2];
        uint16_t ext_len = __read_u16(buf + 3);

        if (ext_len > KCPMUX_PROTO_EXT_MAX_LEN) return -KCPMUX_ERR_INVALID_FORMAT;
        if (size < 5 + ext_len) return -KCPMUX_ERR_INVALID_FORMAT;

        conn->last_recv_ts = recv_time_ms;

        if (result == KCPMUX_ACK_RESULT_OK) {
            // Connection success
            // Save remote returned proto_ext
            if (ext_len > 0) {
                memcpy(conn->peer_proto_ext_buf, buf + 5, ext_len);
                conn->peer_proto_ext.data = conn->peer_proto_ext_buf;
                conn->peer_proto_ext.len = ext_len;
            }
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTED);
        } else {
            // Connection rejected
            // Update statistics
            engine->stats.conn_rejected_total++;
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_ERROR);
            if (result == KCPMUX_ACK_RESULT_VERSION) {
                kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_VERSION);
            } else {
                kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_REJECTED);
            }
        }
        return 0;
    }

    case KCPMUX_MSG_CONN_KEEPALIVE: {
        if (!conn) return -KCPMUX_ERR_NOT_FOUND;
        if (size < 9) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + time(4) + seq(4)

        conn->last_recv_ts = recv_time_ms;
        return 0;
    }

    case KCPMUX_MSG_CONN_CLOSE: {
        if (!conn) return -KCPMUX_ERR_NOT_FOUND;
        if (size < 2) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + reason(1)

        uint8_t reason = buf[1];
        conn->last_recv_ts = recv_time_ms;

        // Send close ack
        kcpmux_conn_send_close_ack(conn, reason);

        // Close connection
        kcpmux_conn_close_internal(conn, reason);
        return 0;
    }

    case KCPMUX_MSG_CONN_CLOSE_ACK: {
        if (!conn || conn->state != KCPMUX_CONN_STATE_CLOSING) return KCPMUX_ERR_STATE;
        if (size < 2) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + reason(1)

        // Received close ack, close connection
        kcpmux_conn_close_internal(conn, conn->close_reason);
        return 0;
    }

    case KCPMUX_MSG_STREAM_CLOSE: {
        if (!conn) return -KCPMUX_ERR_NOT_FOUND;
        if (size < 5) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + stream_id(3) + reason(1)

        uint32_t stream_id = __read_u24(buf + 1);
        if (stream_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;
        uint8_t reason = buf[4];

        kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, stream_id);
        if (!stream) return -KCPMUX_ERR_NOT_FOUND;

        conn->last_recv_ts = recv_time_ms;

        // Send close ack
        kcpmux_stream_send_close_ack(stream, reason);

        // Close stream
        kcpmux_stream_close_internal(stream, reason);
        return 0;
    }

    case KCPMUX_MSG_STREAM_CLOSE_ACK: {
        if (!conn) return -KCPMUX_ERR_NOT_FOUND;
        if (size < 5) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + stream_id(3) + reason(1)

        uint32_t stream_id = __read_u24(buf + 1);
        if (stream_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, stream_id);
        if (!stream || stream->state != KCPMUX_STREAM_STATE_CLOSING) return KCPMUX_ERR_STATE;

        // Received close ack
        kcpmux_stream_close_internal(stream, stream->close_reason);
        return 0;
    }

    case KCPMUX_MSG_STREAM_PAYLOAD: {
        if (!conn || conn->state != KCPMUX_CONN_STATE_CONNECTED) return KCPMUX_ERR_STATE;
        if (size < 4) return -KCPMUX_ERR_INVALID_FORMAT;  // type(1) + stream_id(3)

        uint32_t stream_id = __read_u24(buf + 1);
        if (stream_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, stream_id);
        if (!stream) {
            // Peer should use the opposite stream_id parity.
            if ((stream_id % 2) == conn->is_initiator) return -KCPMUX_ERR_INVALID_FORMAT;

            if (!conn->callbacks.stream_create_notify) {
                return -KCPMUX_ERR_NOT_FOUND;
            }

            stream = kcpmux_stream_new(conn, stream_id, NULL, 0);  // is_initiator = 0
            if (!stream) return -KCPMUX_ERR_OOM;

            kcpmux_conn_add_stream(conn, stream);

            conn->callbacks.stream_create_notify(stream, conn->user_data);

            // First payload processing
            int ret = kcpmux_stream_handle_payload(stream, buf + 4, size - 4, recv_time_ms);
            if (ret != 0) {
                kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_ERROR);
                kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_ERROR);
            } else {
                conn->last_recv_ts = recv_time_ms;
                conn->last_payload_ts = recv_time_ms;
            }
            return ret;
        }

        conn->last_recv_ts = recv_time_ms;
        conn->last_payload_ts = recv_time_ms;

        // Pass to stream for processing (strip message header)
        return kcpmux_stream_handle_payload(stream, buf + 4, size - 4, recv_time_ms);
    }

    default:
        return -KCPMUX_ERR_INVALID_FORMAT;
    }
}

// ============================================================================
// Message send implementation
// ============================================================================

int kcpmux_protocol_send_conn_connect(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_connect: type(1) + version(1) + ext_len(2) + ext(N)
    unsigned msg_len = 4 + conn->self_proto_ext.len;
    if (msg_len > KCPMUX_PROTO_MSG_MAX_LEN) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t buf[KCPMUX_PROTO_MSG_MAX_LEN];
    buf[0] = KCPMUX_MSG_CONN_CONNECT;
    buf[1] = KCPMUX_VERSION;
    __write_u16(buf + 2, (uint16_t)conn->self_proto_ext.len);
    if (conn->self_proto_ext.len > 0) {
        memcpy(buf + 4, conn->self_proto_ext_buf, conn->self_proto_ext.len);
    }

    return kcpmux_conn_write_socket(conn, buf, msg_len);
}

int kcpmux_protocol_send_conn_connect_ack(kcpmux_conn_t *conn, uint8_t result) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_connect_ack: type(1) + version(1) + result(1) + ext_len(2) + ext(N)
    unsigned msg_len = 5 + conn->self_proto_ext.len;
    if (msg_len > KCPMUX_PROTO_MSG_MAX_LEN) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t buf[KCPMUX_PROTO_MSG_MAX_LEN];
    buf[0] = KCPMUX_MSG_CONN_CONNECT_ACK;
    buf[1] = KCPMUX_VERSION;
    buf[2] = result;
    __write_u16(buf + 3, (uint16_t)conn->self_proto_ext.len);
    if (conn->self_proto_ext.len > 0) {
        memcpy(buf + 5, conn->self_proto_ext_buf, conn->self_proto_ext.len);
    }

    return kcpmux_conn_write_socket(conn, buf, msg_len);
}

int kcpmux_protocol_send_conn_keepalive(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_keepalive: type(1) + time(4) + seq(4)
    uint8_t buf[9];
    buf[0] = KCPMUX_MSG_CONN_KEEPALIVE;
    __write_u32(buf + 1, (uint32_t)kcpmux_engine_now(conn->engine));
    __write_u32(buf + 5, conn->keepalive_seq);

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_conn_close(kcpmux_conn_t *conn, uint8_t reason) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_close: type(1) + reason(1)
    uint8_t buf[2];
    buf[0] = KCPMUX_MSG_CONN_CLOSE;
    buf[1] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_conn_close_ack(kcpmux_conn_t *conn, uint8_t reason) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_close_ack: type(1) + reason(1)
    uint8_t buf[2];
    buf[0] = KCPMUX_MSG_CONN_CLOSE_ACK;
    buf[1] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_stream_close(kcpmux_stream_t *stream, uint8_t reason) {
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_conn_t *conn = stream->conn;

    // stream_close: type(1) + stream_id(3) + reason(1)
    uint8_t buf[5];
    buf[0] = KCPMUX_MSG_STREAM_CLOSE;
    __write_u24(buf + 1, stream->stream_id);
    buf[4] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_stream_close_ack(kcpmux_stream_t *stream, uint8_t reason) {
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_conn_t *conn = stream->conn;

    // stream_close_ack: type(1) + stream_id(3) + reason(1)
    uint8_t buf[5];
    buf[0] = KCPMUX_MSG_STREAM_CLOSE_ACK;
    __write_u24(buf + 1, stream->stream_id);
    buf[4] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_stream_payload(kcpmux_stream_t *stream, const uint8_t *kcp_data, unsigned size) {
    if (!stream || !kcp_data || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_conn_t *conn = stream->conn;

    // stream_payload: type(1) + stream_id(3) + kcp_data(N)
    unsigned msg_len = 4 + size;
    if (msg_len > KCPMUX_PROTO_MSG_MAX_LEN) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t buf[KCPMUX_PROTO_MSG_MAX_LEN];
    buf[0] = KCPMUX_MSG_STREAM_PAYLOAD;
    __write_u24(buf + 1, stream->stream_id);
    memcpy(buf + 4, kcp_data, size);

    return kcpmux_conn_write_socket(conn, buf, msg_len);
}
