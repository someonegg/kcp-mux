#ifndef __KCPMUX_H__
#define __KCPMUX_H__

#include "kcpmux_types.h"
#include "kcpmux_kcp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuration defaults.

void kcpmux_engine_config_init(kcpmux_engine_config_t *config);
void kcpmux_conn_config_init(kcpmux_conn_config_t *config);
void kcpmux_stream_config_init(kcpmux_stream_config_t *config);

// Engine API

// callbacks, set_timer, and monotonic_time_ms are required. write_socket is
// required for transport; log_write and conn_connect_notify are optional. A
// custom kcp_ops must provide every operation. Invalid defaults return NULL.
kcpmux_engine_t *kcpmux_engine_create(
    const kcpmux_engine_config_t *config,
    const kcpmux_conn_config_t *default_conn_config,
    const kcpmux_stream_config_t *default_stream_config,
    const kcpmux_engine_callbacks_t *callbacks,
    void *user_data,
    const kcpmux_kcp_ops_t *kcp_ops);

// Cascades close and release to all owned connections and streams.
void kcpmux_engine_destroy(kcpmux_engine_t *engine);

void kcpmux_engine_set_config(kcpmux_engine_t *engine, const kcpmux_engine_config_t *config);

// Processes one UDP packet. Returns 0 on success or a negative error code.
int kcpmux_engine_input(
    kcpmux_engine_t *engine,
    const uint8_t *buf,
    unsigned size,
    const kcpmux_addr_t *peer_addr);

// Requests immediate KCP updates for all streams with operations accumulated
// below their batch thresholds. Call once after an application input batch.
void kcpmux_engine_finish_batch(kcpmux_engine_t *engine);

// Dispatches work due when the replaceable one-shot timer fires.
void kcpmux_engine_update(kcpmux_engine_t *engine);

kcpmux_conn_t *kcpmux_engine_get_conn_by_addr(kcpmux_engine_t *engine, const kcpmux_addr_t *addr);

void kcpmux_engine_get_stats(kcpmux_engine_t *engine, kcpmux_engine_stats_t *stats);

// Connection API

// Engines own connections, and connections own streams. Once an object reaches
// CLOSED or ERROR, it is removed and released automatically. A terminal handle
// becomes invalid when its close callback returns. Query APIs (getters and stats)
// are safe in callbacks while the handle is valid. Unless explicitly allowed by
// the callback declaration, callbacks must defer calls that mutate kcpmux state
// to a later loop.
//
// Close callbacks may run synchronously from engine_input(), engine_update(),
// conn/stream close, engine destruction, or conn_connect() replacing a CLOSING
// connection. After these calls return, previously held conn/stream handles may
// be invalid; query APIs do not advance object lifecycles.

// Creates the initiator connection for an address. Returns NULL while an
// existing connection is CONNECTING or CONNECTED. An existing CLOSING
// connection is synchronously closed with KCPMUX_CLOSE_REASON_REPLACED and a
// distinct non-zero generation is installed; its close callback runs inside
// this call before the new CONNECT is sent. Invalid configuration returns NULL.
kcpmux_conn_t *kcpmux_conn_connect(
    kcpmux_engine_t *engine,
    const kcpmux_addr_t *peer_addr,
    const kcpmux_conn_config_t *config,
    const kcpmux_proto_ext_t *proto_ext,
    const kcpmux_conn_callbacks_t *callbacks,
    void *user_data);

// Starts a terminal close that cascades to all streams. Returns 0 on success or
// a negative error code.
int kcpmux_conn_close(kcpmux_conn_t *conn);

// Invalid configuration is ignored. A zero keepalive timeout uses the default.
void kcpmux_conn_set_config(kcpmux_conn_t *conn, const kcpmux_conn_config_t *config);

void kcpmux_conn_set_callbacks(
    kcpmux_conn_t *conn,
    const kcpmux_conn_callbacks_t *callbacks,
    void *user_data);

uint8_t kcpmux_conn_get_state(kcpmux_conn_t *conn);

kcpmux_engine_t *kcpmux_conn_get_engine(kcpmux_conn_t *conn);

const kcpmux_addr_t *kcpmux_conn_get_peer_addr(kcpmux_conn_t *conn);

void *kcpmux_conn_get_user_data(kcpmux_conn_t *conn);

const kcpmux_proto_ext_t *kcpmux_conn_get_peer_proto_ext(kcpmux_conn_t *conn);

const kcpmux_proto_ext_t *kcpmux_conn_get_self_proto_ext(kcpmux_conn_t *conn);

void kcpmux_conn_get_stats(kcpmux_conn_t *conn, kcpmux_conn_stats_t *stats);

// Stream API

// Invalid configuration returns NULL.
kcpmux_stream_t *kcpmux_stream_create(
    kcpmux_conn_t *conn,
    const kcpmux_stream_config_t *config,
    const kcpmux_stream_callbacks_t *callbacks,
    void *user_data);

// Starts a terminal close. Returns 0 on success or a negative error code.
int kcpmux_stream_close(kcpmux_stream_t *stream);

// Updates runtime configuration except kcp_mss, which is fixed at creation.
// Invalid configuration is ignored.
void kcpmux_stream_set_config(kcpmux_stream_t *stream, const kcpmux_stream_config_t *config);

void kcpmux_stream_set_callbacks(
    kcpmux_stream_t *stream,
    const kcpmux_stream_callbacks_t *callbacks,
    void *user_data);

// Sends data, splitting input larger than kcp_mss into multiple KCP messages;
// in that case one send does not correspond to one recv.
// A nonzero flush requests immediate output. Returns bytes sent, 0 when write
// blocked, or a negative error code.
int kcpmux_stream_send(kcpmux_stream_t *stream, const uint8_t *buf, unsigned size, int flush);

// Requests an immediate KCP update for operations accumulated below the stream
// batch threshold. Does nothing when the stream has no pending operations.
void kcpmux_stream_finish_batch(kcpmux_stream_t *stream);

// Returns the next complete message size, 0 if unavailable, or a negative error.
int kcpmux_stream_peek_size(kcpmux_stream_t *stream);

// Receives one complete message. Use kcpmux_stream_peek_size() to size the
// buffer. Returns bytes received, 0 if unavailable, or a negative error.
int kcpmux_stream_recv(kcpmux_stream_t *stream, uint8_t *buf, unsigned size);

uint32_t kcpmux_stream_id(kcpmux_stream_t *stream);

uint8_t kcpmux_stream_get_state(kcpmux_stream_t *stream);

void *kcpmux_stream_get_kcp(kcpmux_stream_t *stream);

kcpmux_conn_t *kcpmux_stream_get_conn(kcpmux_stream_t *stream);

void *kcpmux_stream_get_user_data(kcpmux_stream_t *stream);

void kcpmux_stream_get_stats(kcpmux_stream_t *stream, kcpmux_stream_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // __KCPMUX_H__
