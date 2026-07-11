#ifndef __KCPMUX_STREAM_H__
#define __KCPMUX_STREAM_H__

#include "kcpmux_types.h"
#include "kcpmux_kcp.h"
#include "kcpmux_list.h"

// ============================================================================
// Stream internal structure
// ============================================================================

struct kcpmux_stream_s {
    list_head                 hash_node;       // Hash table node

    uint32_t                  stream_id;       // Stream ID
    uint8_t                   state;           // Stream state
    uint8_t                   is_initiator;    // Is stream initiator
    uint8_t                   internal_closed; // If internal closed
    kcpmux_conn_t             *conn;            // Owning connection
    kcpmux_stream_config_t     config;          // Stream config
    kcpmux_stream_callbacks_t  callbacks;       // Stream callbacks
    void                     *user_data;       // User data

    void                     *kcp;             // KCP instance

    // Retry state
    int64_t                   last_ctrl_ts;    // Last control message send time
    uint32_t                  retry_count;     // Retry count

    // Close state
    int64_t                   close_ts;        // Close initiation time
    uint8_t                   close_reason;    // Close reason

    // Next update timestamp
    int64_t                   next_update_ts;  // Next update timestamp (ms)

    // Flow control state
    uint8_t                   read_blocked;    // Is read blocked
    uint8_t                   write_blocked;   // Is write blocked

    // Block start timestamps (for statistics)
    int64_t                   read_block_start_ts;   // Read block start time (ms)
    int64_t                   write_block_start_ts;  // Write block start time (ms)

    // Statistics
    kcpmux_stream_stats_t      stats;
};

// ============================================================================
// Stream internal functions
// ============================================================================

// Create stream (internal use)
kcpmux_stream_t *kcpmux_stream_new(kcpmux_conn_t *conn,
                                 uint32_t stream_id,
                                 const kcpmux_stream_config_t *config,
                                 uint8_t is_initiator);

// Close stream internally (does not free memory)
void kcpmux_stream_close_internal(kcpmux_stream_t *stream, uint8_t reason);

// Update stream state
void kcpmux_stream_update(kcpmux_stream_t *stream, int64_t now);

// Get next check interval
int64_t kcpmux_stream_check_interval(kcpmux_stream_t *stream, int64_t now);

// State change
void kcpmux_stream_set_state(kcpmux_stream_t *stream, uint8_t new_state);

// Handle received messages
// Return: 0 on success, < 0 on error
int kcpmux_stream_handle_payload(kcpmux_stream_t *stream, const uint8_t *buf, unsigned size, int64_t recv_time_ms);

// Send messages
// Return: 0 on success, < 0 on error
int kcpmux_stream_send_close(kcpmux_stream_t *stream, uint8_t reason);
int kcpmux_stream_send_close_ack(kcpmux_stream_t *stream, uint8_t reason);

#endif // __KCPMUX_STREAM_H__
