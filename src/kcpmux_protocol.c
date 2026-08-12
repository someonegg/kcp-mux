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
    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, peer_addr);

    if (msg_type == KCPMUX_MSG_CONN_CONNECT) {
        kcpmux_conn_t *new_conn;
        uint32_t generation_id;
        uint8_t version;
        uint16_t ext_len;
        kcpmux_proto_ext_t resp_ext = {0};
        int result = KCPMUX_ACK_RESULT_ERROR;

        if (size < 7) return -KCPMUX_ERR_INVALID_FORMAT;
        generation_id = __read_u24(buf + 1);
        if (generation_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;
        version = buf[4];
        ext_len = __read_u16(buf + 5);

        if (ext_len > KCPMUX_PROTO_EXT_MAX_LEN) return -KCPMUX_ERR_INVALID_FORMAT;
        if (size < 7U + ext_len) return -KCPMUX_ERR_INVALID_FORMAT;

        if (version != KCPMUX_VERSION) {
            if (conn && conn->generation_id == generation_id) {
                kcpmux_conn_send_connect_ack(conn, KCPMUX_ACK_RESULT_VERSION);
            }
            return -KCPMUX_ERR_NOK;
        }

        if (conn && conn->generation_id == generation_id) {
            conn->stats.rx_packets++;
            conn->stats.rx_bytes += size;
            kcpmux_conn_note_receive(conn, recv_time_ms, 0);
            kcpmux_conn_send_connect_ack(conn, KCPMUX_ACK_RESULT_OK);
            return 0;
        }

        new_conn = kcpmux_conn_new(engine, peer_addr, NULL, 0);
        if (!new_conn) return -KCPMUX_ERR_OOM;
        new_conn->generation_id = generation_id;

        if (ext_len > 0) {
            memcpy(new_conn->peer_proto_ext_buf, buf + 7, ext_len);
            new_conn->peer_proto_ext.data = new_conn->peer_proto_ext_buf;
            new_conn->peer_proto_ext.len = ext_len;
        }
        kcpmux_conn_note_receive(new_conn, recv_time_ms, 0);

        // Construct the replacement fully before disturbing the live mapping.
        if (conn) {
            kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_REPLACED);
        }
        kcpmux_engine_add_conn(engine, new_conn);

        if (engine->callbacks.conn_connect_notify) {
            result = engine->callbacks.conn_connect_notify(
                new_conn, &new_conn->peer_proto_ext, &resp_ext,
                engine->user_data);
        }
        if (result == KCPMUX_ACK_RESULT_OK) {
            if (resp_ext.data && resp_ext.len > 0) {
                unsigned len = resp_ext.len;
                if (len > KCPMUX_PROTO_EXT_MAX_LEN) len = KCPMUX_PROTO_EXT_MAX_LEN;
                memcpy(new_conn->self_proto_ext_buf, resp_ext.data, len);
                new_conn->self_proto_ext.data = new_conn->self_proto_ext_buf;
                new_conn->self_proto_ext.len = len;
            }
            kcpmux_conn_set_state(new_conn, KCPMUX_CONN_STATE_CONNECTED);
            kcpmux_conn_send_connect_ack(new_conn, KCPMUX_ACK_RESULT_OK);
        } else {
            engine->stats.conn_rejected_total++;
            kcpmux_conn_send_connect_ack(new_conn, (uint8_t)result);
            memset(&new_conn->callbacks, 0, sizeof(new_conn->callbacks));
            new_conn->user_data = NULL;
            kcpmux_conn_close_internal(new_conn, KCPMUX_CLOSE_REASON_REJECTED);
        }
        return 0;
    }

    // Every non-CONNECT packet belongs to an address and generation pair.
    if (size < 4) return -KCPMUX_ERR_INVALID_FORMAT;
    if (!conn || conn->generation_id != __read_u24(buf + 1)) {
        return -KCPMUX_ERR_NOT_FOUND;
    }
    conn->stats.rx_packets++;
    conn->stats.rx_bytes += size;

    switch (msg_type) {
    case KCPMUX_MSG_CONN_CONNECT_ACK: {
        if (!conn || conn->state != KCPMUX_CONN_STATE_CONNECTING) return KCPMUX_ERR_STATE;
        if (size < 8) return -KCPMUX_ERR_INVALID_FORMAT;

        uint8_t version = buf[4];
        uint8_t result = buf[5];
        uint16_t ext_len = __read_u16(buf + 6);

        if (ext_len > KCPMUX_PROTO_EXT_MAX_LEN) return -KCPMUX_ERR_INVALID_FORMAT;
        if (size < 8U + ext_len) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_conn_note_receive(conn, recv_time_ms, 0);

        if (version == KCPMUX_VERSION && result == KCPMUX_ACK_RESULT_OK) {
            if (ext_len > 0) {
                memcpy(conn->peer_proto_ext_buf, buf + 8, ext_len);
                conn->peer_proto_ext.data = conn->peer_proto_ext_buf;
                conn->peer_proto_ext.len = ext_len;
            }
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTED);
        } else {
            engine->stats.conn_rejected_total++;
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_ERROR);
            if (version != KCPMUX_VERSION || result == KCPMUX_ACK_RESULT_VERSION) {
                kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_VERSION);
            } else {
                kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_REJECTED);
            }
        }
        return 0;
    }

    case KCPMUX_MSG_CONN_KEEPALIVE: {
        if (size < 12) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_conn_note_receive(conn, recv_time_ms, 0);
        return 0;
    }

    case KCPMUX_MSG_CONN_CLOSE: {
        if (size < 5) return -KCPMUX_ERR_INVALID_FORMAT;

        uint8_t reason = buf[4];
        kcpmux_conn_note_receive(conn, recv_time_ms, 0);

        // Send close ack
        kcpmux_conn_send_close_ack(conn, reason);

        // Close connection
        kcpmux_conn_close_internal(conn, reason);
        return 0;
    }

    case KCPMUX_MSG_CONN_CLOSE_ACK: {
        if (!conn || conn->state != KCPMUX_CONN_STATE_CLOSING) return KCPMUX_ERR_STATE;
        if (size < 5) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_conn_note_receive(conn, recv_time_ms, 0);

        // Received close ack, close connection
        kcpmux_conn_close_internal(conn, conn->close_reason);
        return 0;
    }

    case KCPMUX_MSG_STREAM_CLOSE: {
        if (size < 9) return -KCPMUX_ERR_INVALID_FORMAT;

        uint32_t stream_id = __read_u32(buf + 4);
        if (stream_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;
        uint8_t reason = buf[8];

        kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, stream_id);
        if (!stream) return -KCPMUX_ERR_NOT_FOUND;

        kcpmux_conn_note_receive(conn, recv_time_ms, 0);

        // Send close ack
        kcpmux_stream_send_close_ack(stream, reason);

        // Close stream
        kcpmux_stream_close_internal(stream, reason);
        return 0;
    }

    case KCPMUX_MSG_STREAM_CLOSE_ACK: {
        if (size < 9) return -KCPMUX_ERR_INVALID_FORMAT;

        uint32_t stream_id = __read_u32(buf + 4);
        if (stream_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, stream_id);
        if (!stream || stream->state != KCPMUX_STREAM_STATE_CLOSING) return KCPMUX_ERR_STATE;

        kcpmux_conn_note_receive(conn, recv_time_ms, 0);

        // Received close ack
        kcpmux_stream_close_internal(stream, stream->close_reason);
        return 0;
    }

    case KCPMUX_MSG_STREAM_PAYLOAD: {
        if (!conn || conn->state != KCPMUX_CONN_STATE_CONNECTED) return KCPMUX_ERR_STATE;
        if (size < 8) return -KCPMUX_ERR_INVALID_FORMAT;

        uint32_t stream_id = __read_u32(buf + 4);
        if (stream_id == 0) return -KCPMUX_ERR_INVALID_FORMAT;

        kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, stream_id);
        if (!stream) {
            if ((stream_id % 2) == conn->is_initiator) return -KCPMUX_ERR_INVALID_FORMAT;
            if (!kcpmux_conn_is_new_peer_stream_id(conn, stream_id)) {
                return -KCPMUX_ERR_NOT_FOUND;
            }

            if (!conn->callbacks.stream_create_notify) {
                return -KCPMUX_ERR_NOT_FOUND;
            }

            stream = kcpmux_stream_new(conn, stream_id, NULL, 0);
            if (!stream) return -KCPMUX_ERR_OOM;

            kcpmux_conn_add_stream(conn, stream);
            conn->latest_peer_stream_id = stream_id;
            conn->peer_stream_id_initialized = 1;

            int accepted = conn->callbacks.stream_create_notify(
                stream, conn->user_data);
            if (accepted != 0) {
                memset(&stream->callbacks, 0, sizeof(stream->callbacks));
                stream->user_data = NULL;
                stream->close_reason = KCPMUX_CLOSE_REASON_REJECTED;
                stream->retry_count = 0;
                stream->last_ctrl_ts = recv_time_ms;
                kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_CLOSING);
                kcpmux_stream_send_close(
                    stream, KCPMUX_CLOSE_REASON_REJECTED);
                if (stream->config.close_retries == 0) {
                    kcpmux_stream_close_internal(
                        stream, KCPMUX_CLOSE_REASON_REJECTED);
                }
                return 0;
            }

            int ret = kcpmux_stream_handle_payload(
                stream, buf + 8, size - 8, recv_time_ms);
            if (ret != 0) {
                kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_ERROR);
                kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_ERROR);
            } else {
                kcpmux_conn_note_receive(conn, recv_time_ms, 1);
            }
            return ret;
        }

        int ret = kcpmux_stream_handle_payload(
            stream, buf + 8, size - 8, recv_time_ms);
        if (ret == 0) {
            kcpmux_conn_note_receive(conn, recv_time_ms, 1);
        }
        return ret;
    }

    default:
        return -KCPMUX_ERR_INVALID_FORMAT;
    }
}

// ============================================================================
// Message send implementation
// ============================================================================

static inline void __write_common(uint8_t *buf, uint8_t type,
                                  const kcpmux_conn_t *conn) {
    buf[0] = type;
    __write_u24(buf + 1, conn->generation_id);
}

int kcpmux_protocol_send_conn_connect(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_connect: common(4) + version(1) + ext_len(2) + ext(N)
    unsigned msg_len = 7 + conn->self_proto_ext.len;
    if (msg_len > KCPMUX_PROTO_MSG_MAX_LEN) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t buf[KCPMUX_PROTO_MSG_MAX_LEN];
    __write_common(buf, KCPMUX_MSG_CONN_CONNECT, conn);
    buf[4] = KCPMUX_VERSION;
    __write_u16(buf + 5, (uint16_t)conn->self_proto_ext.len);
    if (conn->self_proto_ext.len > 0) {
        memcpy(buf + 7, conn->self_proto_ext_buf, conn->self_proto_ext.len);
    }

    return kcpmux_conn_write_socket(conn, buf, msg_len);
}

int kcpmux_protocol_send_conn_connect_ack(kcpmux_conn_t *conn, uint8_t result) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_connect_ack: common(4) + version(1) + result(1) + ext_len(2) + ext(N)
    unsigned msg_len = 8 + conn->self_proto_ext.len;
    if (msg_len > KCPMUX_PROTO_MSG_MAX_LEN) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t buf[KCPMUX_PROTO_MSG_MAX_LEN];
    __write_common(buf, KCPMUX_MSG_CONN_CONNECT_ACK, conn);
    buf[4] = KCPMUX_VERSION;
    buf[5] = result;
    __write_u16(buf + 6, (uint16_t)conn->self_proto_ext.len);
    if (conn->self_proto_ext.len > 0) {
        memcpy(buf + 8, conn->self_proto_ext_buf, conn->self_proto_ext.len);
    }

    return kcpmux_conn_write_socket(conn, buf, msg_len);
}

int kcpmux_protocol_send_conn_keepalive(kcpmux_conn_t *conn) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_keepalive: common(4) + time(4) + seq(4)
    uint8_t buf[12];
    __write_common(buf, KCPMUX_MSG_CONN_KEEPALIVE, conn);
    __write_u32(buf + 4, (uint32_t)kcpmux_engine_now(conn->engine));
    __write_u32(buf + 8, conn->keepalive_seq);

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_conn_close(kcpmux_conn_t *conn, uint8_t reason) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_close: common(4) + reason(1)
    uint8_t buf[5];
    __write_common(buf, KCPMUX_MSG_CONN_CLOSE, conn);
    buf[4] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_conn_close_ack(kcpmux_conn_t *conn, uint8_t reason) {
    if (!conn) return -KCPMUX_ERR_INVALID_PARAM;

    // conn_close_ack: common(4) + reason(1)
    uint8_t buf[5];
    __write_common(buf, KCPMUX_MSG_CONN_CLOSE_ACK, conn);
    buf[4] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_stream_close(kcpmux_stream_t *stream, uint8_t reason) {
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_conn_t *conn = stream->conn;

    // stream_close: common(4) + stream_id(4) + reason(1)
    uint8_t buf[9];
    __write_common(buf, KCPMUX_MSG_STREAM_CLOSE, conn);
    __write_u32(buf + 4, stream->stream_id);
    buf[8] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_stream_close_ack(kcpmux_stream_t *stream, uint8_t reason) {
    if (!stream) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_conn_t *conn = stream->conn;

    // stream_close_ack: common(4) + stream_id(4) + reason(1)
    uint8_t buf[9];
    __write_common(buf, KCPMUX_MSG_STREAM_CLOSE_ACK, conn);
    __write_u32(buf + 4, stream->stream_id);
    buf[8] = reason;

    return kcpmux_conn_write_socket(conn, buf, sizeof(buf));
}

int kcpmux_protocol_send_stream_payload(kcpmux_stream_t *stream, const uint8_t *kcp_data, unsigned size) {
    if (!stream || !kcp_data || size == 0) return -KCPMUX_ERR_INVALID_PARAM;

    kcpmux_conn_t *conn = stream->conn;

    // stream_payload: common(4) + stream_id(4) + kcp_data(N)
    unsigned msg_len = 8 + size;
    if (msg_len > KCPMUX_PROTO_MSG_MAX_LEN) return -KCPMUX_ERR_INVALID_PARAM;

    uint8_t buf[KCPMUX_PROTO_MSG_MAX_LEN];
    __write_common(buf, KCPMUX_MSG_STREAM_PAYLOAD, conn);
    __write_u32(buf + 4, stream->stream_id);
    memcpy(buf + 8, kcp_data, size);

    return kcpmux_conn_write_socket(conn, buf, msg_len);
}
