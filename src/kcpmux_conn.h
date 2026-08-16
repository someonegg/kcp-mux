#ifndef __KCPMUX_CONN_H__
#define __KCPMUX_CONN_H__

#include "kcpmux_engine.h"
#include "kcpmux_hashtable.h"

// ============================================================================
// Connection internal structure
// ============================================================================

struct kcpmux_conn_s {
    list_head                 hash_node;       // Hash table node

    uint8_t                   state;           // Connection state
    uint8_t                   is_initiator;    // Is connection initiator
    uint8_t                   internal_closed; // If internal closed
    uint8_t                   in_engine_map;   // Hash membership guard
    uint8_t                   peer_stream_id_initialized;
    uint32_t                  generation_id;   // 24-bit wire generation
    kcpmux_engine_t          *engine;          // Owning engine
    kcpmux_conn_config_t      config;          // Connection config
    kcpmux_conn_callbacks_t   callbacks;       // Connection callbacks
    void                     *user_data;       // User data

    // Protocol extension data
    uint8_t                   peer_proto_ext_buf[KCPMUX_PROTO_EXT_MAX_LEN];
    kcpmux_proto_ext_t        peer_proto_ext;  // data points to peer_proto_ext_buf
    uint8_t                   self_proto_ext_buf[KCPMUX_PROTO_EXT_MAX_LEN];
    kcpmux_proto_ext_t        self_proto_ext;  // data points to self_proto_ext_buf

    // Peer address
    kcpmux_addr_t             peer_addr;       // Peer address (points to peer_addr_buf)
    uint8_t                   peer_addr_buf[KCPMUX_ADDR_MAX_LEN];

    // Stream management
    kcpmux_htb_t             *stream_map;      // Stream hash table (key: stream_id)
    uint32_t                  stream_count;    // Stream count
    uint32_t                  next_stream_id;  // Next stream ID
    uint32_t                  latest_peer_stream_id;

    // Keepalive state
    int64_t                   last_recv_ts;      // Last message receive time (for keepalive timeout)
    int64_t                   last_keepalive_ts; // Last keepalive send time
    uint32_t                  keepalive_seq;     // Keepalive sequence number

    // Connection creation time
    int64_t                   created_ts;        // Connection creation time (for handshake time)

    // Idle state
    int64_t                   last_payload_ts;   // Last payload receive time (for idle timeout)

    // Retry state
    int64_t                   last_ctrl_ts;      // Last control message send time
    uint32_t                  retry_count;       // Retry count

    // Close state
    uint8_t                   close_reason;      // Close reason

    // Absolute deadline timer
    kcpmux_timer_node_t       timer_node;
    kcpmux_pending_release_t  pending_release;

    // Statistics
    kcpmux_conn_stats_t       stats;             // Connection statistics
};

// ============================================================================
// Connection internal functions
// ============================================================================

// Create connection (internal use)
kcpmux_conn_t *kcpmux_conn_new(
    kcpmux_engine_t *engine,
    const kcpmux_addr_t *peer_addr,
    const kcpmux_conn_config_t *config,
    uint8_t is_initiator);

// Finalize a connection and queue its physical release.
void kcpmux_conn_close_internal(kcpmux_conn_t *conn, uint8_t reason);

// Update connection state
void kcpmux_conn_update(kcpmux_conn_t *conn, int64_t now);

// Refresh the connection's absolute deadline from its current state/anchors.
void kcpmux_conn_refresh_timer(kcpmux_conn_t *conn, int64_t now);

// Record a successfully handled packet and optionally a stream payload.
void kcpmux_conn_note_receive(kcpmux_conn_t *conn, int64_t recv_time_ms, uint8_t is_payload);

// State change
void kcpmux_conn_set_state(kcpmux_conn_t *conn, uint8_t new_state);

// Send messages
// Return: 0 on success, < 0 on error
int kcpmux_conn_send_connect(kcpmux_conn_t *conn);
int kcpmux_conn_send_connect_ack(kcpmux_conn_t *conn, uint8_t result);
int kcpmux_conn_send_keepalive(kcpmux_conn_t *conn);
int kcpmux_conn_send_close(kcpmux_conn_t *conn, uint8_t reason);
int kcpmux_conn_send_close_ack(kcpmux_conn_t *conn, uint8_t reason);

// Send conn data to socket
// Return: 0 on success, < 0 on error
int kcpmux_conn_write_socket(kcpmux_conn_t *conn, const uint8_t *buf, unsigned size);
int kcpmux_conn_write_socketv(
    kcpmux_conn_t *conn,
    const kcpmux_iovec_t *iov,
    unsigned iovcnt);

// Stream management
void kcpmux_conn_add_stream(kcpmux_conn_t *conn, kcpmux_stream_t *stream);
void kcpmux_conn_remove_stream(kcpmux_conn_t *conn, kcpmux_stream_t *stream);
kcpmux_stream_t *kcpmux_conn_get_stream_by_id(kcpmux_conn_t *conn, uint32_t stream_id);
// Allocate stream_id
uint32_t kcpmux_conn_alloc_stream_id(kcpmux_conn_t *conn);

// Return non-zero when an unknown peer stream ID may be created.
int kcpmux_conn_is_new_peer_stream_id(const kcpmux_conn_t *conn, uint32_t stream_id);

#endif // __KCPMUX_CONN_H__
