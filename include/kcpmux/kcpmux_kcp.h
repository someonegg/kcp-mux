#ifndef __KCPMUX_IKCP_H__
#define __KCPMUX_IKCP_H__

#include <stdint.h>
#include "kcpmux_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// KCP abstraction.

typedef struct kcpmux_kcp_ops_s {
    // Operations are synchronous and must not reenter kcpmux. The installed
    // output callback may only reach the engine write_socket callback.

    // Creates an opaque KCP instance. engine_user is the engine user data.
    void *(*create)(uint32_t kcp_conv, void *kcp_user, void *engine_user);

    void (*release)(void *kcp);

    void (*setmss)(void *kcp, int mss);

    // Installs the lower-layer output callback.
    void (*setoutput)(void *kcp, int (*output)(const char *buf, int len, void *kcp, void *user));

    // Returns 0 on success or -1 on error.
    int (*send)(void *kcp, const char *buf, int len);

    // Returns 0 on success or -1 on error.
    int (*input)(void *kcp, const char *data, long size);

    // Consumes and returns received bytes, or <= 0 if unavailable or invalid.
    int (*recv)(void *kcp, char *buf, int len);

    // Returns readable bytes without consuming them.
    int (*peeksize)(void *kcp);

    // Returns the number of segments waiting to be sent.
    int (*waitsnd)(void *kcp);

    // update advances state to current; check returns the next update time.
    void (*update)(void *kcp, int64_t current);
    int64_t (*check)(void *kcp, int64_t current);

    int64_t (*current)(void *kcp);
    void (*current_update)(void *kcp, int64_t current);

} kcpmux_kcp_ops_t;

// Default KCP implementation.

#define KCPMUX_IKCP_OVERHEAD                  (24)

// ikcp_nodelay(nodelay, interval, faskack, !flowctrl);
#define KCPMUX_KCP_NODELAY                    (0)
#define KCPMUX_KCP_NODELAY_INTERVAL           (10)
#define KCPMUX_KCP_NODELAY_FASTACK            (3)
#define KCPMUX_KCP_NODELAY_FLOWCTRL           (1)

kcpmux_kcp_ops_t *kcpmux_default_kcp_ops(void);

#ifdef __cplusplus
}
#endif

#endif // __KCPMUX_IKCP_H__
