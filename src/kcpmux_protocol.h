#ifndef __KCPMUX_PROTOCOL_H__
#define __KCPMUX_PROTOCOL_H__

#include "kcpmux_types.h"

// ============================================================================
// Message types
// ============================================================================

#define KCPMUX_MSG_CONN_CONNECT       0x81
#define KCPMUX_MSG_CONN_CONNECT_ACK   0x82
#define KCPMUX_MSG_CONN_KEEPALIVE     0x83
#define KCPMUX_MSG_CONN_CLOSE         0x84
#define KCPMUX_MSG_CONN_CLOSE_ACK     0x85
#define KCPMUX_MSG_STREAM_CLOSE       0x93
#define KCPMUX_MSG_STREAM_CLOSE_ACK   0x94
#define KCPMUX_MSG_STREAM_PAYLOAD     0xa1

// Maximum protocol message length (stack buffer size)
#define KCPMUX_PROTO_MSG_MAX_LEN      1500

// Wire lengths: type(1) + destination_connection_id(8).
#define KCPMUX_PROTO_COMMON_LEN       9U
#define KCPMUX_PROTO_CONNECT_LEN      (KCPMUX_PROTO_COMMON_LEN + 11U)
#define KCPMUX_PROTO_CONNECT_ACK_LEN  (KCPMUX_PROTO_COMMON_LEN + 12U)
#define KCPMUX_PROTO_KEEPALIVE_LEN    (KCPMUX_PROTO_COMMON_LEN + 8U)
#define KCPMUX_PROTO_CONN_CLOSE_LEN   (KCPMUX_PROTO_COMMON_LEN + 1U)
#define KCPMUX_PROTO_STREAM_HEADER_LEN (KCPMUX_PROTO_COMMON_LEN + 4U)
#define KCPMUX_PROTO_STREAM_CLOSE_LEN (KCPMUX_PROTO_STREAM_HEADER_LEN + 1U)

// ============================================================================
// Protocol handling functions
// ============================================================================

// Process input packet
// Return: 0 on success, < 0 on error
int kcpmux_protocol_input(
    kcpmux_engine_t *engine,
    const uint8_t *buf,
    unsigned size,
    const kcpmux_addr_t *peer_addr,
    int64_t recv_time_ms);

// ============================================================================
// Message send functions
// ============================================================================

// Connection messages
// Return: 0 on success, < 0 on error
int kcpmux_protocol_send_conn_connect(kcpmux_conn_t *conn);
int kcpmux_protocol_send_conn_connect_ack(kcpmux_conn_t *conn, uint8_t result);
int kcpmux_protocol_send_conn_keepalive(kcpmux_conn_t *conn);
int kcpmux_protocol_send_conn_close(kcpmux_conn_t *conn, uint8_t reason);
int kcpmux_protocol_send_conn_close_ack(kcpmux_conn_t *conn, uint8_t reason);

// Stream messages
// Return: 0 on success, < 0 on error
int kcpmux_protocol_send_stream_close(kcpmux_stream_t *stream, uint8_t reason);
int kcpmux_protocol_send_stream_close_ack(kcpmux_stream_t *stream, uint8_t reason);
int kcpmux_protocol_send_stream_payload(kcpmux_stream_t *stream, const uint8_t *buf, unsigned size);

#endif // __KCPMUX_PROTOCOL_H__
