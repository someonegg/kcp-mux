#include "kcpmux_kcp.h"
#include "ikcp.h"

// ========================================================================
// Wrapper Function
// ========================================================================

static void *kcpmux_default_ikcp_create(uint32_t kcp_conv, void *kcp_user, void *engine_user)
{
    (void)engine_user;
    ikcpcb *kcp = ikcp_create(kcp_conv, kcp_user);
    if (!kcp) {
        return NULL;
    }
    ikcp_nodelay(
        kcp,
        KCPMUX_KCP_NODELAY,
        KCPMUX_KCP_NODELAY_INTERVAL,
        KCPMUX_KCP_NODELAY_FASTACK,
        KCPMUX_KCP_NODELAY_FLOWCTRL);
    return kcp;
}

static void kcpmux_default_ikcp_setmss(void *kcp, int mss)
{
    ikcp_setmtu((ikcpcb *)kcp, mss + KCPMUX_IKCP_OVERHEAD);
}

static void kcpmux_default_ikcp_release(void *kcp)
{
    ikcp_release((ikcpcb *)kcp);
}

static void kcpmux_default_ikcp_setoutput(
    void *kcp,
    int (*output)(const char *buf, int len, void *kcp, void *user))
{
    ikcp_setoutput((ikcpcb *)kcp, output);
}

static int kcpmux_default_ikcp_send(void *kcp, const char *buf, int len)
{
    return ikcp_send((ikcpcb *)kcp, buf, len);
}

static int kcpmux_default_ikcp_input(void *kcp, const char *data, long size)
{
    return ikcp_input((ikcpcb *)kcp, data, size);
}

static int kcpmux_default_ikcp_recv(void *kcp, char *buf, int len)
{
    return ikcp_recv((ikcpcb *)kcp, buf, len);
}

static int kcpmux_default_ikcp_peeksize(void *kcp)
{
    return ikcp_peeksize((const ikcpcb *)kcp);
}

static int kcpmux_default_ikcp_waitsnd(void *kcp)
{
    return ikcp_waitsnd((const ikcpcb *)kcp);
}

static void kcpmux_default_ikcp_update(void *kcp, int64_t current)
{
    ikcp_update((ikcpcb *)kcp, current);
}

static int64_t kcpmux_default_ikcp_check(void *kcp, int64_t current)
{
    return ikcp_check((const ikcpcb *)kcp, current);
}

static int64_t kcpmux_default_ikcp_current(void *kcp)
{
    return ikcp_current((ikcpcb *)kcp);
}

static void kcpmux_default_ikcp_current_update(void *kcp, int64_t current)
{
    ikcp_current_update((ikcpcb *)kcp, current);
}

static int kcpmux_default_ikcp_sendv(
    void *kcp,
    const kcpmux_iovec_t *iov,
    unsigned iovcnt,
    unsigned first_offset,
    int len)
{
    return ikcp_sendv((ikcpcb *)kcp, iov, iovcnt, first_offset, len);
}

kcpmux_kcp_ops_t *kcpmux_default_kcp_ops(void)
{
    static kcpmux_kcp_ops_t default_ikcp_ops = {
        .create    = kcpmux_default_ikcp_create,
        .release   = kcpmux_default_ikcp_release,
        .setmss    = kcpmux_default_ikcp_setmss,
        .setoutput = kcpmux_default_ikcp_setoutput,
        .send      = kcpmux_default_ikcp_send,
        .input     = kcpmux_default_ikcp_input,
        .recv      = kcpmux_default_ikcp_recv,
        .peeksize  = kcpmux_default_ikcp_peeksize,
        .waitsnd   = kcpmux_default_ikcp_waitsnd,
        .update    = kcpmux_default_ikcp_update,
        .check     = kcpmux_default_ikcp_check,
        .current   = kcpmux_default_ikcp_current,
        .current_update = kcpmux_default_ikcp_current_update,
        .sendv     = kcpmux_default_ikcp_sendv,
    };
    return &default_ikcp_ops;
}
