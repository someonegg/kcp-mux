#ifndef __KCPMUX_IKCP_H__
#define __KCPMUX_IKCP_H__

#include <stdint.h>
#include "kcpmux_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// KCP Abstraction
// ============================================================================

typedef struct kcpmux_kcp_ops_s {
    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Create kcp instance (e.g., KCP)
    // kcp_*: ikcp_create(kcp_conv, kcp_user)
    // engine_user: engine user data pointer
    // Returns: kcp instance handle (opaque pointer)
    void* (*create)(uint32_t kcp_conv, void *kcp_user, void *engine_user);

    // Destroy kcp instance
    void (*release)(void *kcp);

    // ========================================================================
    // Data Transfer
    // ========================================================================

    void (*setmss)(void *kcp, int mss);

    // Set output callback for sending data to lower layer
    // output: callback function to send data
    // Returns: 0 on success, -1 on error
    void (*setoutput)(void *kcp,
                     int (*output)(const char *buf, int len, void *kcp, void *user));

    // Send data to kcp
    // Returns: 0 on success, -1 on error
    int (*send)(void *kcp, const char *buf, int len);

    // Input data from network into kcp
    // Returns: 0 on success, -1 on error
    int (*input)(void *kcp, const char *data, long size);

    // Receive data from kcp (consumes data)
    // Returns: bytes received, <= 0 if no data or error
    int (*recv)(void *kcp, char *buf, int len);

    // ========================================================================
    // State Query
    // ========================================================================

    // Get readable data size (without consuming)
    // Returns: readable bytes, 0 if no data
    int (*peeksize)(void *kcp);

    // Get waiting send data size
    // Returns: number of segments waiting to be sent
    int (*waitsnd)(void *kcp);

    // ========================================================================
    // Timer
    // ========================================================================

    // update state (call it repeatedly, every 10ms-100ms), or you can ask
    // 'check' when to call it again (without send/input calling).
    // current: current timestamp in millisec.
    void (*update)(void *kcp, int64_t current);
    int64_t (*check)(void *kcp, int64_t current);

    int64_t (*current)(void *kcp);
    void (*current_update)(void *kcp, int64_t current);

} kcpmux_kcp_ops_t;

// ============================================================================
// Default KCP Implementation
// ============================================================================

#define KCPMUX_IKCP_OVERHEAD                  (24)

// ikcp_nodelay(nodelay, interval, faskack, !flowctrl);
#define KCPMUX_KCP_NODELAY                    (0)
#define KCPMUX_KCP_NODELAY_INTERVAL           (10)
#define KCPMUX_KCP_NODELAY_FASTACK            (3)
#define KCPMUX_KCP_NODELAY_FLOWCTRL           (1)

// Get default KCP ops.
kcpmux_kcp_ops_t* kcpmux_default_kcp_ops(void);

#ifdef __cplusplus
}
#endif

#endif // __KCPMUX_IKCP_H__
