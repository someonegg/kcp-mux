#ifndef __KCPMUX_ENGINE_H__
#define __KCPMUX_ENGINE_H__

#include "kcpmux_types.h"
#include "kcpmux_kcp.h"
#include "kcpmux_hashtable.h"

// ============================================================================
// Engine internal structure
// ============================================================================

struct kcpmux_engine_s {
    kcpmux_engine_config_t     config;          // Engine config
    kcpmux_engine_callbacks_t  callbacks;       // Engine callbacks
    void                     *user_data;       // User data

    kcpmux_kcp_ops_t         *kcp_ops;         // KCP operations

    // Default configurations for conn/stream
    kcpmux_conn_config_t       default_conn_config;      // Default conn config
    kcpmux_stream_config_t     default_stream_config;    // Default stream config

    // Connection management
    kcpmux_htb_t              *conn_map;        // Connection hash table (key: peer_addr)
    uint32_t                  conn_count;      // Connection count
    kcpmux_engine_stats_t      stats;           // Engine statistics
};

// ============================================================================
// Engine internal functions
// ============================================================================

// Get current time (ms)
int64_t kcpmux_engine_now(kcpmux_engine_t *engine);

// Schedule next wakeup
void kcpmux_engine_schedule_timer(kcpmux_engine_t *engine, int64_t now);

// Send data to socket
// Return: 0 on success, < 0 on error
int kcpmux_engine_write_socket(kcpmux_engine_t *engine,
                              const uint8_t *buf, unsigned size,
                              const kcpmux_addr_t *addr);

// Add/remove connection
void kcpmux_engine_add_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn);
void kcpmux_engine_remove_conn(kcpmux_engine_t *engine, kcpmux_conn_t *conn);

#endif // __KCPMUX_ENGINE_H__
