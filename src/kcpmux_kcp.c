#include "kcpmux_kcp.h"
#include "ikcp.h"

// ========================================================================
// Wrapper Function
// ========================================================================

static void* kcpmux_kcp_create(uint32_t kcp_conv, void *kcp_user, void *engine_user) {
    (void)engine_user;    // User pointer is available for future use
    ikcpcb *kcp = ikcp_create(kcp_conv, kcp_user);
    ikcp_nodelay(kcp, KCPMUX_KCP_NODELAY, KCPMUX_KCP_NODELAY_INTERVAL, KCPMUX_KCP_NODELAY_FASTACK, KCPMUX_KCP_NODELAY_FLOWCTRL);
    return kcp;
}

static void kcpmux_kcp_setmss(void *kcp, int mss) {
    ikcp_setmtu(kcp, mss + KCPMUX_IKCP_OVERHEAD);
}

kcpmux_kcp_ops_t* kcpmux_default_kcp_ops(void) {
        static kcpmux_kcp_ops_t default_kcp_ops = {
        .create    = kcpmux_kcp_create,
        .release   = (void*) ikcp_release,
        .setmss    = kcpmux_kcp_setmss,
        .setoutput = (void*) ikcp_setoutput,
        .send      = (void*) ikcp_send,
        .input     = (void*) ikcp_input,
        .recv      = (void*) ikcp_recv,
        .peeksize  = (void*) ikcp_peeksize,
        .waitsnd   = (void*) ikcp_waitsnd,
        .update    = (void*) ikcp_update,
        .check     = (void*) ikcp_check,
        .current   = (void*) ikcp_current,
        .current_update   = (void*) ikcp_current_update,
    };
    return &default_kcp_ops;
}
