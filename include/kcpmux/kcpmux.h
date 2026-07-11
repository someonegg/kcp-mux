#ifndef __KCPMUX_H__
#define __KCPMUX_H__

#include "kcpmux_types.h"
#include "kcpmux_kcp.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration init functions
// ============================================================================

void kcpmux_engine_config_init(kcpmux_engine_config_t *config);
void kcpmux_conn_config_init(kcpmux_conn_config_t *config);
void kcpmux_stream_config_init(kcpmux_stream_config_t *config);

// ============================================================================
// Engine API
// ============================================================================

// Create engine
kcpmux_engine_t *kcpmux_engine_create(
    const kcpmux_engine_config_t *config,
    const kcpmux_conn_config_t *default_conn_config,
    const kcpmux_stream_config_t *default_stream_config,
    const kcpmux_engine_callbacks_t *callbacks,
    void *user_data,
    const kcpmux_kcp_ops_t * kcp_ops);

// Destroy engine (cascade free all connections)
void kcpmux_engine_destroy(kcpmux_engine_t *engine);

// Set engine configuration
void kcpmux_engine_set_config(kcpmux_engine_t *engine,
                              const kcpmux_engine_config_t *config);

// Process received UDP packet
// Return: 0 on success, < 0 on error
int kcpmux_engine_input(kcpmux_engine_t *engine,
                       const uint8_t *buf, unsigned size,
                       const kcpmux_addr_t *peer_addr);

// Drive main logic (call periodically)
void kcpmux_engine_update(kcpmux_engine_t *engine);

// Find connection by address
kcpmux_conn_t *kcpmux_engine_get_conn_by_addr(
    kcpmux_engine_t *engine,
    const kcpmux_addr_t *addr);

// Get engine statistics
void kcpmux_engine_get_stats(kcpmux_engine_t *engine, kcpmux_engine_stats_t *stats);

// ============================================================================
// Connection API
// ============================================================================

// Initiate connection
kcpmux_conn_t *kcpmux_conn_connect(
    kcpmux_engine_t *engine,
    const kcpmux_addr_t *peer_addr,
    const kcpmux_conn_config_t *config,
    const kcpmux_proto_ext_t *proto_ext,
    const kcpmux_conn_callbacks_t *callbacks,
    void *user_data);

// Close connection (cascade close all streams)
// Return: 0 on success, < 0 on error
int kcpmux_conn_close(kcpmux_conn_t *conn);

// Free connection memory (will auto-close if not closed, cascade free all streams)
void kcpmux_conn_free(kcpmux_conn_t *conn);

// Set connection configuration
void kcpmux_conn_set_config(kcpmux_conn_t *conn,
                           const kcpmux_conn_config_t *config);

// Set callbacks and user data
void kcpmux_conn_set_callbacks(kcpmux_conn_t *conn,
                              const kcpmux_conn_callbacks_t *callbacks,
                              void *user_data);

// Get connection state
uint8_t kcpmux_conn_get_state(kcpmux_conn_t *conn);

// Get owning engine
kcpmux_engine_t *kcpmux_conn_get_engine(kcpmux_conn_t *conn);

// Get peer address
const kcpmux_addr_t *kcpmux_conn_get_peer_addr(kcpmux_conn_t *conn);

// Get user data
void *kcpmux_conn_get_user_data(kcpmux_conn_t *conn);

// Get peer protocol extension data
const kcpmux_proto_ext_t *kcpmux_conn_get_peer_proto_ext(kcpmux_conn_t *conn);

// Get self protocol extension data
const kcpmux_proto_ext_t *kcpmux_conn_get_self_proto_ext(kcpmux_conn_t *conn);

// Get connection statistics
void kcpmux_conn_get_stats(kcpmux_conn_t *conn, kcpmux_conn_stats_t *stats);

// ============================================================================
// Stream API
// ============================================================================

// Create stream
kcpmux_stream_t *kcpmux_stream_create(
    kcpmux_conn_t *conn,
    const kcpmux_stream_config_t *config,
    const kcpmux_stream_callbacks_t *callbacks,
    void *user_data);

// Close stream
// Return: 0 on success, < 0 on error
int kcpmux_stream_close(kcpmux_stream_t *stream);

// Free stream memory (will auto-close if not closed)
void kcpmux_stream_free(kcpmux_stream_t *stream);

// Set stream configuration
void kcpmux_stream_set_config(kcpmux_stream_t *stream,
                             const kcpmux_stream_config_t *config);

// Set callbacks and user data
void kcpmux_stream_set_callbacks(kcpmux_stream_t *stream,
                                const kcpmux_stream_callbacks_t *callbacks,
                                void *user_data);

// Send data
// Parameters:
//   flush - if non-zero, flush immediately; otherwise, flushed periodically by KCP update
// Returns:
//   > 0: number of bytes sent
//   = 0: send buffer full (write blocked), wait for stream_write_notify callback
//   < 0: error code (KCPMUX_ERR_CLOSED, KCPMUX_ERR_STATE, etc.)
int kcpmux_stream_send(kcpmux_stream_t *stream,
                      const uint8_t *buf, unsigned size,
                      int flush);

// Receive data
// Returns:
//   > 0: number of bytes received
//   = 0: no data available or error, wait for stream_read_notify callback
//   < 0: error code (KCPMUX_ERR_CLOSED, KCPMUX_ERR_STATE, etc.)
int kcpmux_stream_recv(kcpmux_stream_t *stream,
                      uint8_t *buf, unsigned size);

// Get stream ID
uint32_t kcpmux_stream_id(kcpmux_stream_t *stream);

// Get stream state
uint8_t kcpmux_stream_get_state(kcpmux_stream_t *stream);

// Get stream KCP
void* kcpmux_stream_get_kcp(kcpmux_stream_t *stream);

// Get owning connection
kcpmux_conn_t *kcpmux_stream_get_conn(kcpmux_stream_t *stream);

// Get user data
void *kcpmux_stream_get_user_data(kcpmux_stream_t *stream);

// Get stream statistics
void kcpmux_stream_get_stats(kcpmux_stream_t *stream, kcpmux_stream_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // __KCPMUX_H__
