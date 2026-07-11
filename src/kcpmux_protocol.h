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

// 24-bit stream ID max value
#define KCPMUX_STREAM_ID_MAX          0xFFFFFF

// ============================================================================
// Protocol handling functions
// ============================================================================

// Process input packet
// Return: 0 on success, < 0 on error
int kcpmux_protocol_input(kcpmux_engine_t *engine,
                         const uint8_t *buf, unsigned size,
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
