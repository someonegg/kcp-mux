#ifndef __KCPMUX_TYPES_H__
#define __KCPMUX_TYPES_H__

#include <stdint.h>

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct kcpmux_engine_s kcpmux_engine_t;
typedef struct kcpmux_conn_s kcpmux_conn_t;
typedef struct kcpmux_stream_s kcpmux_stream_t;

// ============================================================================
// Protocol constants
// ============================================================================

#define KCPMUX_VERSION                 2       // Protocol version
#define KCPMUX_ADDR_MAX_LEN            64      // Max address length
#define KCPMUX_PROTO_EXT_MAX_LEN       512     // Max extension data length

// ============================================================================
// Address structure
// ============================================================================

typedef struct kcpmux_addr_s {
    uint8_t    *addr;                 // Address data
    unsigned    addrlen;              // Address data length
} kcpmux_addr_t;

// ============================================================================
// Status codes
// ============================================================================

// Common status codes.
#define KCPMUX_ERR_OK                        0
#define KCPMUX_ERR_NOK                       1
#define KCPMUX_ERR_TIMEOUT                   2
#define KCPMUX_ERR_CANCEL                    3
#define KCPMUX_ERR_READY                     4
#define KCPMUX_ERR_QUIT                      5
#define KCPMUX_ERR_SKIP                      6
#define KCPMUX_ERR_OVERFLOW                  7
#define KCPMUX_ERR_OOM                       8
#define KCPMUX_ERR_EOF                       9
#define KCPMUX_ERR_NOT_FOUND                 10
#define KCPMUX_ERR_DUP                       11
#define KCPMUX_ERR_PARTIAL                   12
#define KCPMUX_ERR_NREADY                    13
#define KCPMUX_ERR_IDLE                      14
#define KCPMUX_ERR_CORRUPTED                 15
#define KCPMUX_ERR_TBD                       16
#define KCPMUX_ERR_EAGAIN                    17
#define KCPMUX_ERR_REDIRECT                  18
#define KCPMUX_ERR_NETWORK                   19
#define KCPMUX_ERR_NIL_PTR                   40
#define KCPMUX_ERR_INVALID_FORMAT            41
#define KCPMUX_ERR_INVALID_PARAM             42

// kcpmux-specific negative API errors.
#define KCPMUX_ERR_STATE                     -200    // mismatch state
#define KCPMUX_ERR_CLOSED                    -201    // already closed
#define KCPMUX_ERR_KCPRET(kcp_ret)          (-300 + (kcp_ret))

// ACK Result codes (connection handshake)
#define KCPMUX_ACK_RESULT_OK                 0x00    // Success
#define KCPMUX_ACK_RESULT_ERROR              0x01    // Error
#define KCPMUX_ACK_RESULT_VERSION            0x02    // Version mismatch
#define KCPMUX_ACK_RESULT_CUSTOM             0x10    // 0x10-0xff, upper layer custom code

// Close Reason codes (conn / stream)
#define KCPMUX_CLOSE_REASON_NORMAL           0x00    // Normal close
#define KCPMUX_CLOSE_REASON_ERROR            0x01    // Error close
#define KCPMUX_CLOSE_REASON_VERSION          0x02    // Version mismatch
#define KCPMUX_CLOSE_REASON_REJECTED         0x03    // Rejected by upper layer
#define KCPMUX_CLOSE_REASON_IDLE             0x04    // Idle Close
#define KCPMUX_CLOSE_REASON_TIMEOUT          0x05    // Timeout

// ============================================================================
// Connection state
// ============================================================================

typedef enum {
    KCPMUX_CONN_STATE_INIT = 0,        // Initial state
    KCPMUX_CONN_STATE_CONNECTING,      // Connecting (waiting for ack)
    KCPMUX_CONN_STATE_CONNECTED,       // Connected (can send/recv data)
    KCPMUX_CONN_STATE_CLOSING,         // Closing (waiting for ack or timeout)
    KCPMUX_CONN_STATE_CLOSED,          // Closed
    KCPMUX_CONN_STATE_ERROR,           // Error
} kcpmux_conn_state_t;

// ============================================================================
// Stream state
// ============================================================================

typedef enum {
    KCPMUX_STREAM_STATE_OPEN = 0,      // Open (can send/recv)
    KCPMUX_STREAM_STATE_CLOSING,       // Closing (waiting for ack or timeout)
    KCPMUX_STREAM_STATE_CLOSED,        // Closed
    KCPMUX_STREAM_STATE_ERROR,         // Error
} kcpmux_stream_state_t;

// ============================================================================
// Protocol extension data
// ============================================================================

typedef struct kcpmux_proto_ext_s {
    uint8_t  *data;                   // Extension data pointer
    unsigned  len;                    // Extension data length (0 ~ KCPMUX_PROTO_EXT_MAX_LEN)
} kcpmux_proto_ext_t;

// ============================================================================
// Default configuration values
// ============================================================================

// Connection defaults
#define KCPMUX_DEFAULT_CONTROL_TIMEOUT_MS      400     // 400ms
#define KCPMUX_DEFAULT_CONNECT_RETRIES         2       // 2 retries
#define KCPMUX_DEFAULT_CLOSE_RETRIES           1       // 1 retry
#define KCPMUX_DEFAULT_KEEPALIVE_INTERVAL_MS   10000   // 10s
#define KCPMUX_DEFAULT_KEEPALIVE_TIMEOUT_MS    30000   // 30s
#define KCPMUX_DEFAULT_IDLE_TIMEOUT_MS         60000   // 60s

// Stream defaults
#define KCPMUX_DEFAULT_SCONTROL_TIMEOUT_MS     600     // 600ms
#define KCPMUX_DEFAULT_SCLOSE_RETRIES          1       // 1 retry

// KCP defaults
#define KCPMUX_DEFAULT_KCP_MSS                 1200
#define KCPMUX_DEFAULT_SEND_PAUSE_THRESHOLD    256     // Pause sending when waitsnd >= 256
#define KCPMUX_DEFAULT_SEND_RESUME_THRESHOLD   128     // Resume sending when waitsnd < 128

// ============================================================================
// Configuration structures
// ============================================================================

typedef struct kcpmux_engine_config_s {
    int _reserved;                    // Reserved for future use
} kcpmux_engine_config_t;

typedef struct kcpmux_conn_config_s {
    // Control Message
    uint32_t ctrl_timeout_ms;         // Control Message timeout
    uint32_t connect_retries;         // Connect retries
    uint32_t close_retries;           // Close retries

    // Keepalive
    uint32_t keepalive_interval_ms;   // Heartbeat interval
    uint32_t keepalive_timeout_ms;    // Heartbeat response timeout

    // Idle management
    uint32_t idle_timeout_ms;         // Idle timeout (close if no data)
} kcpmux_conn_config_t;

typedef struct kcpmux_stream_config_s {
    // Control Message
    uint32_t ctrl_timeout_ms;         // Control Message timeout
    uint32_t close_retries;           // Close retries

    // KCP parameters
    uint16_t kcp_mss;                 // MTU = ... + MPCKP header (4) + KCP header + mss
    uint32_t send_pause_threshold;    // Pause sending when waitsnd >= this value
    uint32_t send_resume_threshold;   // Resume sending when waitsnd < this value
} kcpmux_stream_config_t;

// ============================================================================
// Statistics structures
// ============================================================================

typedef struct kcpmux_engine_stats_s {
    // Lower layer packet statistics
    uint64_t tx_packets;             // Packets sent to lower layer
    uint64_t tx_bytes;               // Bytes sent to lower layer
    uint64_t rx_packets;             // Packets received from lower layer
    uint64_t rx_bytes;               // Bytes received from lower layer

    // Error statistics
    uint64_t tx_error_packets;       // Packets failed to send (write_socket failed)

    // Connection statistics
    uint64_t conn_count;             // Current connection count
    uint64_t stream_count;           // Current stream count (sum of all connections)

    // Connection lifecycle statistics
    uint64_t conn_created_total;     // Connection created total
    uint64_t conn_closed_total;      // Connection closed (internal) total
    uint64_t conn_connected_total;   // Connection connected total (state changed to CONNECTED)
    uint64_t conn_rejected_total;    // Connection rejected total
                                     // (receiver notify rejected + initiator received reject ACK)
    uint64_t conn_idle_timeout_total;    // Connection idle timeout total
    uint64_t conn_keepalive_timeout_total; // Connection keepalive timeout total

    // Stream lifecycle statistics
    uint64_t stream_created_total;   // Stream created total
    uint64_t stream_closed_total;    // Stream closed (internal) total
    uint64_t stream_opened_total;    // Stream opened total (state changed to OPEN)

    // API call statistics (counted on every call, regardless of success/failure)
    uint64_t api_conn_connect_calls;   // kcpmux_conn_connect call count
    uint64_t api_conn_close_calls;     // kcpmux_conn_close call count
    uint64_t api_stream_create_calls;  // kcpmux_stream_create call count
    uint64_t api_stream_close_calls;   // kcpmux_stream_close call count
    uint64_t api_stream_send_calls;    // kcpmux_stream_send call count
    uint64_t api_stream_recv_calls;    // kcpmux_stream_recv call count
} kcpmux_engine_stats_t;

typedef struct kcpmux_conn_stats_s {
    // Lower layer packet statistics
    uint64_t tx_packets;             // Packets sent to lower layer
    uint64_t tx_bytes;               // Bytes sent to lower layer
    uint64_t rx_packets;             // Packets received from lower layer
    uint64_t rx_bytes;               // Bytes received from lower layer

    // Connection statistics
    uint64_t handshake_time_ms;      // Handshake time (ms) from creation to connected
} kcpmux_conn_stats_t;

typedef struct kcpmux_stream_stats_s {
    // Lower layer packet statistics
    uint64_t tx_packets;             // Packets sent to lower layer
    uint64_t tx_bytes;               // Bytes sent to lower layer
    uint64_t rx_packets;             // Packets received from lower layer
    uint64_t rx_bytes;               // Bytes received from lower layer

    // Upper layer data statistics
    uint64_t up_sent_bytes;          // Bytes sent by upper layer
    uint64_t up_recv_bytes;          // Bytes received by upper layer

    // Block statistics
    uint64_t ttfb_time_ms;           // TTFB
    uint64_t write_block_count;      // Number of times write became blocked
    uint64_t write_block_time_ms;    // Total time spent in write blocked state (milliseconds)
    uint64_t read_block_count;       // Number of times read became blocked
    uint64_t read_block_time_ms;     // Total time spent in read blocked state (milliseconds)
} kcpmux_stream_stats_t;

// ============================================================================
// Callback structures
// ============================================================================

typedef struct kcpmux_engine_callbacks_s {
    // Timer callback - notify upper layer of next wakeup time
    void (*set_timer)(uint64_t wake_after_ms, void *user_data);

    // Send data callback - upper layer handles actual socket send
    // Return: 1 on success, 0 on error
    int (*write_socket)(const uint8_t *buf, unsigned size,
                           const kcpmux_addr_t *addr, void *user_data);

    // Log callback
    void (*log_write)(int level, const char *buf, unsigned size, void *user_data);

    // Get timestamps
    int64_t (*monotonic_time_ms)(void *user_data);

    // Connection notification (remote initiated)
    // Note: conn is already added to engine's lookup table when this callback is invoked.
    // If this callback is NULL, passive connection creation is disabled.
    // Return KCPMUX_ACK_RESULT_OK to accept, other values to reject (conn will be freed).
    int (*conn_connect_notify)(kcpmux_conn_t *conn,
                               const kcpmux_proto_ext_t *proto_ext,
                               kcpmux_proto_ext_t *resp_proto_ext,
                               void *user_data);
} kcpmux_engine_callbacks_t;

typedef struct kcpmux_conn_callbacks_s {
    // Connection state change notification
    void (*conn_state_changed)(kcpmux_conn_t *conn, uint8_t old_state,
                               uint8_t new_state, void *user_data);

    // Connection close notification
    // Note: conn state is CLOSED or ERROR when this callback is invoked.
    // The conn object remains valid after this callback returns.
    // User must call kcpmux_conn_free() to release the connection memory.
    void (*conn_close_notify)(kcpmux_conn_t *conn, int reason, void *user_data);

    // Stream creation notification (remote initiated by payload auto-create)
    // Note: stream is already added to connection's lookup table when this callback is invoked.
    // If this callback is NULL, passive stream creation is disabled.
    void (*stream_create_notify)(kcpmux_stream_t *stream, void *user_data);
} kcpmux_conn_callbacks_t;

typedef struct kcpmux_stream_callbacks_s {
    // Stream state change notification
    void (*stream_state_changed)(kcpmux_stream_t *stream, uint8_t old_state,
                                 uint8_t new_state, void *user_data);

    // Data readable notification
    // Edge-triggered: notified only when state changes from "blocked" to "readable"
    void (*stream_read_notify)(kcpmux_stream_t *stream, void *user_data);

    // Write ready notification
    // Edge-triggered: notified only when state changes from "blocked" to "writable"
    void (*stream_write_notify)(kcpmux_stream_t *stream, void *user_data);

    // Stream close notification
    // Note: stream state is CLOSED or ERROR when this callback is invoked.
    // The stream object remains valid after this callback returns.
    // User must call kcpmux_stream_free() to release the stream memory.
    void (*stream_close_notify)(kcpmux_stream_t *stream, int reason, void *user_data);
} kcpmux_stream_callbacks_t;

#endif // __KCPMUX_TYPES_H__
