//=====================================================================
//
// KCP - A Better ARQ Protocol Implementation
// skywind3000 (at) gmail.com, 2010-2011
//
// Features:
// + Average RTT reduce 30% - 40% vs traditional ARQ like tcp.
// + Maximum RTT reduce three times vs tcp.
// + Lightweight, distributed as a single source file.
//
//=====================================================================
#include "ikcp.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define IKCP_FASTACK_CONSERVE

//=====================================================================
// KCP BASIC
//=====================================================================
const IUINT32 IKCP_RTO_NDL         =  50;    // no delay min rto
const IUINT32 IKCP_RTO_MIN         = 100;    // normal min rto
const IUINT32 IKCP_RTO_DEF         = 200;
const IUINT32 IKCP_RTO_MAX         = 60000;
const IUINT32 IKCP_RTO_G_RED       = 10;
const IUINT32 IKCP_CMD_PUSH        = 81;     // cmd: push data
const IUINT32 IKCP_CMD_ACK         = 82;     // cmd: ack
const IUINT32 IKCP_CMD_WASK        = 83;     // cmd: window probe (ask)
const IUINT32 IKCP_CMD_WINS        = 84;     // cmd: window size (tell)
const IUINT32 IKCP_ASK_SEND        = 1;      // need to send IKCP_CMD_WASK
const IUINT32 IKCP_ASK_TELL        = 2;      // need to send IKCP_CMD_WINS
const IUINT32 IKCP_WND_SND         = 128;
const IUINT32 IKCP_WND_RCV         = 128;    // must >= max fragment size
const IUINT32 IKCP_MTU_DEF         = 1400;
const IUINT32 IKCP_INTERVAL        = 100;
const IUINT32 IKCP_OVERHEAD        = 24;
const IUINT32 IKCP_DEADLINK        = 20;
const IUINT32 IKCP_THRESH_INIT     = 16;
const IUINT32 IKCP_THRESH_MIN      = 16;
const IUINT32 IKCP_CWND_INIT       = 8;
const IUINT32 IKCP_CWND_LOW        = 24;
const IUINT32 IKCP_CWND_MIN        = 4;
const IUINT32 IKCP_CWND_MIN_SAVE   = 8;
const IUINT32 IKCP_CWND_MAX_SAVE   = 32;
const IUINT32 IKCP_PROBE_INIT      = 7000;   // 7 secs to probe window size
const IUINT32 IKCP_PROBE_LIMIT     = 120000; // up to 120 secs to probe window
const IUINT32 IKCP_FASTACK_LIMIT   = 5;      // max times to trigger fastack
const IUINT32 IKCP_REMIND_LIMIT    = 3;      // max times to trigger remind


//---------------------------------------------------------------------
// encode / decode
//---------------------------------------------------------------------

/* encode 8 bits unsigned int */
static inline char *ikcp_encode8u(char *p, unsigned char c)
{
    *(unsigned char*)p++ = c;
    return p;
}

/* decode 8 bits unsigned int */
static inline const char *ikcp_decode8u(const char *p, unsigned char *c)
{
    *c = *(unsigned char*)p++;
    return p;
}

/* encode 16 bits unsigned int (lsb) */
static inline char *ikcp_encode16u(char *p, unsigned short w)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *(unsigned char*)(p + 0) = (w & 255);
    *(unsigned char*)(p + 1) = (w >> 8);
#else
    memcpy(p, &w, 2);
#endif
    p += 2;
    return p;
}

/* decode 16 bits unsigned int (lsb) */
static inline const char *ikcp_decode16u(const char *p, unsigned short *w)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *w = *(const unsigned char*)(p + 1);
    *w = *(const unsigned char*)(p + 0) + (*w << 8);
#else
    memcpy(w, p, 2);
#endif
    p += 2;
    return p;
}

/* encode 32 bits unsigned int (lsb) */
static inline char *ikcp_encode32u(char *p, IUINT32 l)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *(unsigned char*)(p + 0) = (unsigned char)((l >>  0) & 0xff);
    *(unsigned char*)(p + 1) = (unsigned char)((l >>  8) & 0xff);
    *(unsigned char*)(p + 2) = (unsigned char)((l >> 16) & 0xff);
    *(unsigned char*)(p + 3) = (unsigned char)((l >> 24) & 0xff);
#else
    memcpy(p, &l, 4);
#endif
    p += 4;
    return p;
}

/* decode 32 bits unsigned int (lsb) */
static inline const char *ikcp_decode32u(const char *p, IUINT32 *l)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *l = *(const unsigned char*)(p + 3);
    *l = *(const unsigned char*)(p + 2) + (*l << 8);
    *l = *(const unsigned char*)(p + 1) + (*l << 8);
    *l = *(const unsigned char*)(p + 0) + (*l << 8);
#else
    memcpy(l, p, 4);
#endif
    p += 4;
    return p;
}

static inline IUINT32 _imin_(IUINT32 a, IUINT32 b) {
    return a <= b ? a : b;
}

static inline IUINT32 _imax_(IUINT32 a, IUINT32 b) {
    return a >= b ? a : b;
}

static inline IUINT32 _ibound_(IUINT32 lower, IUINT32 middle, IUINT32 upper)
{
    return _imin_(_imax_(lower, middle), upper);
}

static inline long _itimediff(IUINT32 later, IUINT32 earlier)
{
    return ((IINT32)(later - earlier));
}

static inline long _itime64diff(IINT64 later, IINT64 earlier)
{
    return ((IINT32)(later - earlier));
}

//---------------------------------------------------------------------
// manage segment
//---------------------------------------------------------------------
typedef struct IKCPSEG IKCPSEG;

static void* (*ikcp_malloc_hook)(size_t) = NULL;
static void (*ikcp_free_hook)(void *) = NULL;

// internal malloc
static void* ikcp_malloc(size_t size) {
    if (ikcp_malloc_hook)
        return ikcp_malloc_hook(size);
    return malloc(size);
}

// internal free
static void ikcp_free(void *ptr) {
    if (ikcp_free_hook) {
        ikcp_free_hook(ptr);
    }    else {
        free(ptr);
    }
}

// redefine allocator
void ikcp_allocator(void* (*new_malloc)(size_t), void (*new_free)(void*))
{
    ikcp_malloc_hook = new_malloc;
    ikcp_free_hook = new_free;
}

// allocate a new kcp segment
static IKCPSEG* ikcp_segment_new(ikcpcb *kcp, int size)
{
    return (IKCPSEG*)ikcp_malloc(sizeof(IKCPSEG) + size);
}

// delete a segment
static void ikcp_segment_delete(ikcpcb *kcp, IKCPSEG *seg)
{
    ikcp_free(seg);
}

// write log
void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...)
{
    char buffer[1024];
    va_list argptr;
    if ((mask & kcp->logmask) == 0 || kcp->writelog == 0) return;
    va_start(argptr, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);
    kcp->writelog(buffer, kcp, kcp->user);
}

// check log mask
static int ikcp_canlog(const ikcpcb *kcp, int mask)
{
    if ((mask & kcp->logmask) == 0 || kcp->writelog == NULL) return 0;
    return 1;
}

// output segment
static int ikcp_output(ikcpcb *kcp, const void *data, int size)
{
    assert(kcp);
    assert(kcp->output);
    if (ikcp_canlog(kcp, IKCP_LOG_OUTPUT)) {
        ikcp_log(kcp, IKCP_LOG_OUTPUT, "[RO] %ld bytes", (long)size);
    }
    if (size == 0) return 0;
    return kcp->output((const char*)data, size, kcp, kcp->user);
}

static int ikcp_quotah(ikcpcb *kcp, int size)
{
    assert(kcp);
    if (kcp->quotah == NULL) return 0;
    return kcp->quotah(size, kcp, kcp->user);
}

static void ikcp_losth(ikcpcb *kcp, struct IKCPSEG *seg)
{
    assert(kcp);
    if (NULL == kcp->losth) return;
    kcp->losth(kcp, seg, kcp->user);
}

// output queue
void ikcp_qprint(const char *name, const struct IQUEUEHEAD *head)
{
#if 0
    const struct IQUEUEHEAD *p;
    printf("<%s>: [", name);
    for (p = head->next; p != head; p = p->next) {
        const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
        printf("(%lu %d)", (unsigned long)seg->sn, (int)(seg->ts % 10000));
        if (p->next != head) printf(",");
    }
    printf("]\n");
#endif
}


//---------------------------------------------------------------------
// create a new kcpcb
//---------------------------------------------------------------------
ikcpcb* ikcp_create(IUINT32 conv, void *user)
{
    ikcpcb *kcp = (ikcpcb*)ikcp_malloc(sizeof(struct IKCPCB));
    if (kcp == NULL) return NULL;
    kcp->conv = conv;
    kcp->user = user;
    kcp->snd_una = 0;
    kcp->snd_nxt = 0;
    kcp->rcv_nxt = 0;
    kcp->ts_probe = 0;
    kcp->probe_wait = 0;
    kcp->snd_wnd = IKCP_WND_SND;
    kcp->rcv_wnd = IKCP_WND_RCV;
    kcp->rmt_wnd = IKCP_WND_RCV;
    kcp->cwnd = 0;
    kcp->incr = 0;
    kcp->probe = 0;
    kcp->mtu = IKCP_MTU_DEF;
    kcp->mss = kcp->mtu - IKCP_OVERHEAD;
    kcp->stream = 0;

    kcp->buffer = (char*)ikcp_malloc((kcp->mtu + IKCP_OVERHEAD) * 3);
    if (kcp->buffer == NULL) {
        ikcp_free(kcp);
        return NULL;
    }

    iqueue_init(&kcp->snd_queue);
    iqueue_init(&kcp->rcv_queue);
    iqueue_init(&kcp->snd_buf);
    iqueue_init(&kcp->rcv_buf);
    kcp->nrcv_buf = 0;
    kcp->nsnd_buf = 0;
    kcp->nrcv_que = 0;
    kcp->nsnd_que = 0;
    kcp->state = 0;
    kcp->acklist = NULL;
    kcp->ackblock = 0;
    kcp->ackcount = 0;
    kcp->rx_srtt = 0;
    kcp->rx_rttval = 0;
    kcp->rx_rto = IKCP_RTO_DEF;
    kcp->rx_minrto = IKCP_RTO_MIN;
    kcp->ts_current = 0;
    kcp->interval = IKCP_INTERVAL;
    kcp->ts_flush = IKCP_INTERVAL;
    kcp->reminds = 0;
    kcp->nodelay = 0;
    kcp->updated = 0;
    kcp->logmask = 0;
    kcp->ssthresh = IKCP_THRESH_INIT;
    kcp->fastresend = 0;
    kcp->fastlimit = IKCP_FASTACK_LIMIT;
    kcp->nocwnd = 0;
    kcp->xmit = 0;
    kcp->dead_link = IKCP_DEADLINK;
    kcp->output = NULL;
    kcp->losth = NULL;
    kcp->quotah = NULL;
    kcp->writelog = NULL;

    return kcp;
}


//---------------------------------------------------------------------
// release a new kcpcb
//---------------------------------------------------------------------
void ikcp_release(ikcpcb *kcp)
{
    assert(kcp);
    if (kcp) {
        IKCPSEG *seg;
        while (!iqueue_is_empty(&kcp->snd_buf)) {
            seg = iqueue_entry(kcp->snd_buf.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        while (!iqueue_is_empty(&kcp->rcv_buf)) {
            seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        while (!iqueue_is_empty(&kcp->snd_queue)) {
            seg = iqueue_entry(kcp->snd_queue.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        while (!iqueue_is_empty(&kcp->rcv_queue)) {
            seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        if (kcp->buffer) {
            ikcp_free(kcp->buffer);
        }
        if (kcp->acklist) {
            ikcp_free(kcp->acklist);
        }

        kcp->nrcv_buf = 0;
        kcp->nsnd_buf = 0;
        kcp->nrcv_que = 0;
        kcp->nsnd_que = 0;
        kcp->ackcount = 0;
        kcp->buffer = NULL;
        kcp->acklist = NULL;
        ikcp_free(kcp);
    }
}


//---------------------------------------------------------------------
// set output callback, which will be invoked by kcp
//---------------------------------------------------------------------
void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len,
    void *kcp, void *user))
{
    kcp->output = output;
}

void ikcp_setquotah(ikcpcb *kcp, int (*quotah)(int len, ikcpcb *kcp, void *user))
{
    kcp->quotah = quotah;
}

void ikcp_setlosth(ikcpcb *kcp, void (*handle)(ikcpcb *kcp, struct IKCPSEG *seg, void *user))
{
    kcp->losth = handle;
}

//---------------------------------------------------------------------
// user/upper level recv: returns size, returns below zero for EAGAIN
//---------------------------------------------------------------------
int ikcp_recv(ikcpcb *kcp, char *buffer, int len)
{
    struct IQUEUEHEAD *p;
    int ispeek = (len < 0)? 1 : 0;
    int peeksize;
    int recover = 0;
    IKCPSEG *seg;
    assert(kcp);

    if (iqueue_is_empty(&kcp->rcv_queue))
        return -1;

    if (len < 0) len = -len;

    peeksize = ikcp_peeksize(kcp);

    if (peeksize < 0)
        return -2;

    if (peeksize > len)
        return -3;

    if (kcp->nrcv_que >= kcp->rcv_wnd)
        recover = 1;

    // merge fragment
    for (len = 0, p = kcp->rcv_queue.next; p != &kcp->rcv_queue; ) {
        int fragment;
        seg = iqueue_entry(p, IKCPSEG, node);
        p = p->next;

        if (buffer) {
            memcpy(buffer, seg->data, seg->len);
            buffer += seg->len;
        }

        len += seg->len;
        fragment = seg->frg;

        if (ikcp_canlog(kcp, IKCP_LOG_RECV)) {
            ikcp_log(kcp, IKCP_LOG_RECV, "recv sn=%lu", (unsigned long)seg->sn);
        }

        if (ispeek == 0) {
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
            kcp->nrcv_que--;
        }

        if (fragment == 0)
            break;
    }

    assert(len == peeksize);

    // move available data from rcv_buf -> rcv_queue
    while (! iqueue_is_empty(&kcp->rcv_buf)) {
        seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
        if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
            iqueue_del(&seg->node);
            kcp->nrcv_buf--;
            iqueue_add_tail(&seg->node, &kcp->rcv_queue);
            kcp->nrcv_que++;
            kcp->rcv_nxt++;
        }    else {
            break;
        }
    }

    // fast recover
    if (kcp->nrcv_que < kcp->rcv_wnd && recover) {
        // ready to send back IKCP_CMD_WINS in ikcp_flush
        // tell remote my window size
        kcp->probe |= IKCP_ASK_TELL;
        kcp->reminds = IKCP_REMIND_LIMIT; // reset
    }

    return len;
}


//---------------------------------------------------------------------
// peek data size
//---------------------------------------------------------------------
int ikcp_peeksize(const ikcpcb *kcp)
{
    struct IQUEUEHEAD *p;
    IKCPSEG *seg;
    int length = 0;

    assert(kcp);

    if (iqueue_is_empty(&kcp->rcv_queue)) return -1;

    seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
    if (seg->frg == 0) return seg->len;

    if (kcp->nrcv_que < seg->frg + 1) return -1;

    for (p = kcp->rcv_queue.next; p != &kcp->rcv_queue; p = p->next) {
        seg = iqueue_entry(p, IKCPSEG, node);
        length += seg->len;
        if (seg->frg == 0) break;
    }

    return length;
}


//---------------------------------------------------------------------
// user/upper level send, returns below zero for error
//---------------------------------------------------------------------
int ikcp_send(ikcpcb *kcp, const char *buffer, int len)
{
    IKCPSEG *seg;
    int count, i;
    int sent = 0;

    assert(kcp->mss > 0);
    if (len < 0) return -1;

    // append to previous segment in streaming mode (if possible)
    if (kcp->stream != 0) {
        if (!iqueue_is_empty(&kcp->snd_queue)) {
            IKCPSEG *old = iqueue_entry(kcp->snd_queue.prev, IKCPSEG, node);
            if (old->len < kcp->mss) {
                int capacity = kcp->mss - old->len;
                int extend = (len < capacity)? len : capacity;
                seg = ikcp_segment_new(kcp, old->len + extend);
                assert(seg);
                if (seg == NULL) {
                    return -2;
                }
                iqueue_add_tail(&seg->node, &kcp->snd_queue);
                memcpy(seg->data, old->data, old->len);
                if (buffer) {
                    memcpy(seg->data + old->len, buffer, extend);
                    buffer += extend;
                }
                seg->len = old->len + extend;
                seg->frg = 0;
                len -= extend;
                iqueue_del_init(&old->node);
                ikcp_segment_delete(kcp, old);
                sent = extend;
            }
        }
        if (len <= 0) {
            return sent;
        }
    }

    if (len <= (int)kcp->mss) count = 1;
    else count = (len + kcp->mss - 1) / kcp->mss;

    if (count >= (int)IKCP_WND_RCV) {
        if (kcp->stream != 0 && sent > 0)
            return sent;
        return -2;
    }

    if (count == 0) count = 1;

    // fragment
    for (i = 0; i < count; i++) {
        int size = len > (int)kcp->mss ? (int)kcp->mss : len;
        seg = ikcp_segment_new(kcp, size);
        assert(seg);
        if (seg == NULL) {
            return -2;
        }
        if (buffer && len > 0) {
            memcpy(seg->data, buffer, size);
        }
        seg->len = size;
        seg->frg = (kcp->stream == 0)? (count - i - 1) : 0;
        iqueue_init(&seg->node);
        iqueue_add_tail(&seg->node, &kcp->snd_queue);
        kcp->nsnd_que++;
        if (buffer) {
            buffer += size;
        }
        len -= size;
        sent += size;
    }

    return sent;
}


//---------------------------------------------------------------------
// vectored user/upper level send, returns below zero for error
//---------------------------------------------------------------------
typedef struct IKCPIOVCURSOR {
    const kcpmux_iovec_t *iov;
    unsigned index;
    unsigned offset;
} IKCPIOVCURSOR;

static void ikcp_iov_copy(IKCPIOVCURSOR *cursor, char *dest, int len)
{
    while (len > 0) {
        const kcpmux_iovec_t *item = &cursor->iov[cursor->index];
        unsigned available = item->len - cursor->offset;
        unsigned take = available < (unsigned)len ? available : (unsigned)len;

        if (take > 0) {
            memcpy(dest, item->data + cursor->offset, take);
            dest += take;
            len -= (int)take;
            cursor->offset += take;
        }
        if (cursor->offset == item->len) {
            cursor->index++;
            cursor->offset = 0;
        }
    }
}

int ikcp_sendv(ikcpcb *kcp, const kcpmux_iovec_t *iov, unsigned iovcnt,
    unsigned first_offset, int len)
{
    IKCPIOVCURSOR cursor;
    IKCPSEG *seg;
    size_t available = 0;
    int count, i;
    unsigned j;
    int sent = 0;

    assert(kcp->mss > 0);
    if (len < 0) return -1;
    if (len == 0) return ikcp_send(kcp, NULL, 0);
    if (iov == NULL || iovcnt == 0 || first_offset > iov[0].len) return -1;

    for (j = 0; j < iovcnt; j++) {
        unsigned offset = j == 0 ? first_offset : 0;
        if (iov[j].len > 0 && iov[j].data == NULL) return -1;
        available += iov[j].len - offset;
        if (available >= (size_t)len) break;
    }
    if (available < (size_t)len) return -1;

    cursor.iov = iov;
    cursor.index = 0;
    cursor.offset = first_offset;

    // append to previous segment in streaming mode (if possible)
    if (kcp->stream != 0) {
        if (!iqueue_is_empty(&kcp->snd_queue)) {
            IKCPSEG *old = iqueue_entry(kcp->snd_queue.prev, IKCPSEG, node);
            if (old->len < kcp->mss) {
                int capacity = kcp->mss - old->len;
                int extend = (len < capacity)? len : capacity;
                seg = ikcp_segment_new(kcp, old->len + extend);
                assert(seg);
                if (seg == NULL) return -2;
                iqueue_add_tail(&seg->node, &kcp->snd_queue);
                memcpy(seg->data, old->data, old->len);
                ikcp_iov_copy(&cursor, seg->data + old->len, extend);
                seg->len = old->len + extend;
                seg->frg = 0;
                len -= extend;
                iqueue_del_init(&old->node);
                ikcp_segment_delete(kcp, old);
                sent = extend;
            }
        }
        if (len <= 0) return sent;
    }

    if (len <= (int)kcp->mss) count = 1;
    else count = (len + kcp->mss - 1) / kcp->mss;

    if (count >= (int)IKCP_WND_RCV) {
        if (kcp->stream != 0 && sent > 0) return sent;
        return -2;
    }
    if (count == 0) count = 1;

    for (i = 0; i < count; i++) {
        int size = len > (int)kcp->mss ? (int)kcp->mss : len;
        seg = ikcp_segment_new(kcp, size);
        assert(seg);
        if (seg == NULL) return -2;
        if (len > 0) ikcp_iov_copy(&cursor, seg->data, size);
        seg->len = size;
        seg->frg = (kcp->stream == 0)? (count - i - 1) : 0;
        iqueue_init(&seg->node);
        iqueue_add_tail(&seg->node, &kcp->snd_queue);
        kcp->nsnd_que++;
        len -= size;
        sent += size;
    }

    return sent;
}


//---------------------------------------------------------------------
// parse ack
//---------------------------------------------------------------------
static void ikcp_update_ack(ikcpcb *kcp, IINT32 rtt)
{
    IINT32 rto = 0;
    if (kcp->rx_srtt == 0) {
        kcp->rx_srtt = rtt;
        kcp->rx_rttval = rtt / 2;
    }    else {
        long delta = rtt - kcp->rx_srtt;
        if (delta < 0) delta = -delta;
        if (rtt < kcp->rx_srtt - kcp->rx_rttval) {
            kcp->rx_rttval = (31 * kcp->rx_rttval + delta) / 32;
        } else {
            kcp->rx_rttval = (3 * kcp->rx_rttval + delta) / 4;
        }
        kcp->rx_srtt = (7 * kcp->rx_srtt + rtt) / 8;
        if (kcp->rx_srtt < 1) kcp->rx_srtt = 1;
    }
    rto = kcp->rx_srtt + _imax_(kcp->interval + IKCP_RTO_G_RED, 4 * kcp->rx_rttval);
    kcp->rx_rto = _ibound_(kcp->rx_minrto, rto, IKCP_RTO_MAX);
}

static void ikcp_shrink_buf(ikcpcb *kcp)
{
    struct IQUEUEHEAD *p = kcp->snd_buf.next;
    if (p != &kcp->snd_buf) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        kcp->snd_una = seg->sn;
    }    else {
        kcp->snd_una = kcp->snd_nxt;
    }
}

static void ikcp_parse_ack(ikcpcb *kcp, IUINT32 sn)
{
    struct IQUEUEHEAD *p, *next;

    if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0)
        return;

    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        next = p->next;
        if (sn == seg->sn) {
            iqueue_del(p);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
            break;
        }
        if (_itimediff(sn, seg->sn) < 0) {
            break;
        }
    }
}

static void ikcp_parse_una(ikcpcb *kcp, IUINT32 una)
{
    struct IQUEUEHEAD *p, *next;
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        next = p->next;
        if (_itimediff(una, seg->sn) > 0) {
            iqueue_del(p);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
        }    else {
            break;
        }
    }
}

static void ikcp_parse_fastack(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
    struct IQUEUEHEAD *p, *next;

    if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0)
        return;

    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        next = p->next;
        if (_itimediff(sn, seg->sn) < 0) {
            break;
        }
        else if (sn != seg->sn) {
        #ifndef IKCP_FASTACK_CONSERVE
            seg->fastack++;
        #else
            if (_itimediff(ts, seg->ts) >= 0)
                seg->fastack++;
        #endif
        }
    }
}


//---------------------------------------------------------------------
// ack append
//---------------------------------------------------------------------
static void ikcp_ack_push(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
    IUINT32 newsize = kcp->ackcount + 1;
    IUINT32 *ptr;

    if (newsize > kcp->ackblock) {
        IUINT32 *acklist;
        IUINT32 newblock;

        for (newblock = 8; newblock < newsize; newblock <<= 1);
        acklist = (IUINT32*)ikcp_malloc(newblock * sizeof(IUINT32) * 2);

        if (acklist == NULL) {
            assert(acklist != NULL);
            abort();
        }

        if (kcp->acklist != NULL) {
            IUINT32 x;
            for (x = 0; x < kcp->ackcount; x++) {
                acklist[x * 2 + 0] = kcp->acklist[x * 2 + 0];
                acklist[x * 2 + 1] = kcp->acklist[x * 2 + 1];
            }
            ikcp_free(kcp->acklist);
        }

        kcp->acklist = acklist;
        kcp->ackblock = newblock;
    }

    ptr = &kcp->acklist[kcp->ackcount * 2];
    ptr[0] = sn;
    ptr[1] = ts;
    kcp->ackcount++;
}

static void ikcp_ack_get(const ikcpcb *kcp, int p, IUINT32 *sn, IUINT32 *ts)
{
    if (sn) sn[0] = kcp->acklist[p * 2 + 0];
    if (ts) ts[0] = kcp->acklist[p * 2 + 1];
}


//---------------------------------------------------------------------
// parse data
//---------------------------------------------------------------------
void ikcp_parse_data(ikcpcb *kcp, IKCPSEG *newseg)
{
    struct IQUEUEHEAD *p, *prev;
    IUINT32 sn = newseg->sn;
    int repeat = 0;

    if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) >= 0 ||
        _itimediff(sn, kcp->rcv_nxt) < 0) {
        ikcp_segment_delete(kcp, newseg);
        return;
    }

    for (p = kcp->rcv_buf.prev; p != &kcp->rcv_buf; p = prev) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        prev = p->prev;
        if (seg->sn == sn) {
            repeat = 1;
            break;
        }
        if (_itimediff(sn, seg->sn) > 0) {
            break;
        }
    }

    if (repeat == 0) {
        iqueue_init(&newseg->node);
        iqueue_add(&newseg->node, p);
        kcp->nrcv_buf++;
    }    else {
        ikcp_segment_delete(kcp, newseg);
    }

#if 0
    ikcp_qprint("rcvbuf", &kcp->rcv_buf);
    printf("rcv_nxt=%lu\n", kcp->rcv_nxt);
#endif

    // move available data from rcv_buf -> rcv_queue
    while (! iqueue_is_empty(&kcp->rcv_buf)) {
        IKCPSEG *seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
        if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
            iqueue_del(&seg->node);
            kcp->nrcv_buf--;
            iqueue_add_tail(&seg->node, &kcp->rcv_queue);
            kcp->nrcv_que++;
            kcp->rcv_nxt++;
        }    else {
            break;
        }
    }

#if 0
    ikcp_qprint("queue", &kcp->rcv_queue);
    printf("rcv_nxt=%lu\n", kcp->rcv_nxt);
#endif

#if 0
    printf("snd(buf=%d, queue=%d)\n", kcp->nsnd_buf, kcp->nsnd_que);
    printf("rcv(buf=%d, queue=%d)\n", kcp->nrcv_buf, kcp->nrcv_que);
#endif
}


//---------------------------------------------------------------------
// input data
//---------------------------------------------------------------------
int ikcp_input(ikcpcb *kcp, const char *data, long size)
{
    IINT64 current = kcp->ts_current;
    IUINT32 prev_una = kcp->snd_una;
    IUINT32 maxack = 0, latest_ts = 0;
    int flag = 0;

    if (ikcp_canlog(kcp, IKCP_LOG_INPUT)) {
        ikcp_log(kcp, IKCP_LOG_INPUT, "[RI] %d bytes", (int)size);
    }

    if (data == NULL || (int)size < (int)IKCP_OVERHEAD) return -1;

    while (1) {
        IUINT32 ts, sn, len, una, conv;
        IUINT16 wnd;
        IUINT8 cmd, frg;
        IKCPSEG *seg;

        if (size < (int)IKCP_OVERHEAD) break;

        data = ikcp_decode32u(data, &conv);
        if (conv != kcp->conv) return -1;

        data = ikcp_decode8u(data, &cmd);
        data = ikcp_decode8u(data, &frg);
        data = ikcp_decode16u(data, &wnd);
        data = ikcp_decode32u(data, &ts);
        data = ikcp_decode32u(data, &sn);
        data = ikcp_decode32u(data, &una);
        data = ikcp_decode32u(data, &len);

        size -= IKCP_OVERHEAD;

        if ((long)size < (long)len || (int)len < 0) return -2;

        if (cmd != IKCP_CMD_PUSH && cmd != IKCP_CMD_ACK &&
            cmd != IKCP_CMD_WASK && cmd != IKCP_CMD_WINS)
            return -3;

        kcp->rmt_wnd = wnd;
        ikcp_parse_una(kcp, una);
        ikcp_shrink_buf(kcp);

        if (cmd == IKCP_CMD_ACK) {
            if (_itimediff((IUINT32)current, ts) >= 0) {
                ikcp_update_ack(kcp, _itimediff((IUINT32)current, ts));
            }
            ikcp_parse_ack(kcp, sn);
            ikcp_shrink_buf(kcp);
            if (flag == 0) {
                flag = 1;
                maxack = sn;
                latest_ts = ts;
            }    else {
                if (_itimediff(sn, maxack) > 0) {
                #ifndef IKCP_FASTACK_CONSERVE
                    maxack = sn;
                    latest_ts = ts;
                #else
                    if (_itimediff(ts, latest_ts) > 0) {
                        maxack = sn;
                        latest_ts = ts;
                    }
                #endif
                }
            }
            if (ikcp_canlog(kcp, IKCP_LOG_IN_ACK)) {
                ikcp_log(kcp, IKCP_LOG_IN_ACK,
                    "input ack: sn=%lu rtt=%ld rto=%ld ts=%lu", (unsigned long)sn,
                    (long)_itimediff((IUINT32)current, ts),
                    (long)kcp->rx_rto, (unsigned long)ts);
            }
        }
        else if (cmd == IKCP_CMD_PUSH) {
            if (ikcp_canlog(kcp, IKCP_LOG_IN_DATA)) {
                ikcp_log(kcp, IKCP_LOG_IN_DATA,
                    "input psh: sn=%lu ts=%lu", (unsigned long)sn, (unsigned long)ts);
            }

            if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) < 0) {
                ikcp_ack_push(kcp, sn, ts);
                if (_itimediff(sn, kcp->rcv_nxt) >= 0) {
                    seg = ikcp_segment_new(kcp, len);
                    assert(seg);
                    seg->conv = conv;
                    seg->cmd = cmd;
                    seg->frg = frg;
                    seg->wnd = wnd;
                    seg->ts = ts;
                    seg->sn = sn;
                    seg->una = una;
                    seg->len = len;

                    if (len > 0) {
                        memcpy(seg->data, data, len);
                    }

                    ikcp_parse_data(kcp, seg);

                    kcp->reminds = IKCP_REMIND_LIMIT; // reset
                }
            }
        }
        else if (cmd == IKCP_CMD_WASK) {
            // ready to send back IKCP_CMD_WINS in ikcp_flush
            // tell remote my window size
            kcp->probe |= IKCP_ASK_TELL;
            kcp->reminds = IKCP_REMIND_LIMIT; // reset
            if (ikcp_canlog(kcp, IKCP_LOG_IN_PROBE)) {
                ikcp_log(kcp, IKCP_LOG_IN_PROBE, "input probe");
            }
        }
        else if (cmd == IKCP_CMD_WINS) {
            // do nothing
            if (ikcp_canlog(kcp, IKCP_LOG_IN_WINS)) {
                ikcp_log(kcp, IKCP_LOG_IN_WINS,
                    "input wins: %lu", (unsigned long)(wnd));
            }
        }
        else {
            return -3;
        }

        data += len;
        size -= len;
    }

    if (flag != 0) {
        ikcp_parse_fastack(kcp, maxack, latest_ts);
    }

    if (_itimediff(kcp->snd_una, prev_una) > 0) {
        if (kcp->cwnd < kcp->rmt_wnd) {
            IUINT32 mss = kcp->mss;
            if (kcp->cwnd < kcp->ssthresh) {
                kcp->cwnd++;
                kcp->incr += mss;
            }    else {
                if (kcp->incr < mss) kcp->incr = mss;
                kcp->incr += (mss * mss) / kcp->incr + (mss / 16);
                if ((kcp->cwnd + 1) * mss <= kcp->incr) {
                #if 1
                    kcp->cwnd = (kcp->incr + mss - 1) / ((mss > 0)? mss : 1);
                #else
                    kcp->cwnd++;
                #endif
                }
            }
            if (kcp->cwnd > kcp->rmt_wnd) {
                kcp->cwnd = kcp->rmt_wnd;
                kcp->incr = kcp->rmt_wnd * mss;
            }
        }
    }

    return 0;
}

//---------------------------------------------------------------------
// ikcp_encode_seg
//---------------------------------------------------------------------
static char *ikcp_encode_seg(char *ptr, const IKCPSEG *seg)
{
    ptr = ikcp_encode32u(ptr, seg->conv);
    ptr = ikcp_encode8u(ptr, (IUINT8)seg->cmd);
    ptr = ikcp_encode8u(ptr, (IUINT8)seg->frg);
    ptr = ikcp_encode16u(ptr, (IUINT16)seg->wnd);
    ptr = ikcp_encode32u(ptr, seg->ts);
    ptr = ikcp_encode32u(ptr, seg->sn);
    ptr = ikcp_encode32u(ptr, seg->una);
    ptr = ikcp_encode32u(ptr, seg->len);
    return ptr;
}

static int ikcp_wnd_unused(const ikcpcb *kcp)
{
    if (kcp->nrcv_que < kcp->rcv_wnd) {
        return kcp->rcv_wnd - kcp->nrcv_que;
    }
    return 0;
}


//---------------------------------------------------------------------
// update state (call it repeatedly, every 10ms-100ms), or you can ask
// ikcp_check when to call it again (without ikcp_input/_send calling).
// 'current' - current timestamp in millisec.
//---------------------------------------------------------------------
void ikcp_update(ikcpcb *kcp, IINT64 current)
{
    char *buffer = kcp->buffer;
    char *ptr = buffer;
    int count, size, i;
    IUINT32 resent, cwnd;
    IUINT32 rtomin;
    struct IQUEUEHEAD *p;
    int change = 0;
    int lost = 0;
    IKCPSEG seg;
    IUINT32 una = kcp->rcv_nxt, rtt_sn = 0, rtt_ts = 0;

    if (kcp->updated == 0) {
        kcp->updated = 1;
    }
    kcp->ts_current = current;
    kcp->ts_flush = current + kcp->interval;

    seg.conv = kcp->conv;
    seg.cmd = IKCP_CMD_ACK;
    seg.frg = 0;
    seg.wnd = ikcp_wnd_unused(kcp);
    seg.una = una;
    seg.len = 0;
    seg.sn = 0;
    seg.ts = 0;

    // flush acknowledges
    // sn < una 的包可被 UNA 隐式确认，只保留最大序列号的用于 RTT；
    // sn >= una 的乱序包必须显式 ACK 触发 fastack
    count = kcp->ackcount;
    int has_rtt_ack = 0;
    // 找rtt_ack（sn < una 中最大的）
    for (i = 0; i < count; i++) {
        IUINT32 ack_sn, ack_ts;
        ikcp_ack_get(kcp, i, &ack_sn, &ack_ts);
        if (_itimediff(ack_sn, una) < 0) {
            if (!has_rtt_ack || _itimediff(ack_sn, rtt_sn) > 0) {
                rtt_sn = ack_sn;
                rtt_ts = ack_ts;
                has_rtt_ack = 1;
            }
        }
    }
    // 先发送 rtt_ack
    if (has_rtt_ack) {
        seg.sn = rtt_sn;
        seg.ts = rtt_ts;
        ptr = ikcp_encode_seg(ptr, &seg);
    }
    // 发送乱序 ACK（sn >= una）
    for (i = 0; i < count; i++) {
        IUINT32 ack_sn, ack_ts;
        ikcp_ack_get(kcp, i, &ack_sn, &ack_ts);
        if (_itimediff(ack_sn, una) >= 0) {
            size = (int)(ptr - buffer);
            if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
                ikcp_output(kcp, buffer, size);
                ptr = buffer;
            }
            seg.sn = ack_sn;
            seg.ts = ack_ts;
            ptr = ikcp_encode_seg(ptr, &seg);
        }
    }

    kcp->ackcount = 0;

    // probe window size (if remote window size equals zero)
    if (kcp->rmt_wnd == 0) {
        if (kcp->probe_wait == 0) {
            kcp->probe_wait = IKCP_PROBE_INIT;
            kcp->ts_probe = current + kcp->probe_wait;
        }
        else {
            if (_itime64diff(current, kcp->ts_probe) >= 0) {
                if (kcp->probe_wait < IKCP_PROBE_INIT)
                    kcp->probe_wait = IKCP_PROBE_INIT;
                kcp->probe_wait += kcp->probe_wait / 2;
                if (kcp->probe_wait > IKCP_PROBE_LIMIT)
                    kcp->probe_wait = IKCP_PROBE_LIMIT;
                kcp->ts_probe = current + kcp->probe_wait;
                kcp->probe |= IKCP_ASK_SEND;
            }
        }
    }    else {
        kcp->ts_probe = 0;
        kcp->probe_wait = 0;
    }

    // flush window probing commands
    if (kcp->probe & IKCP_ASK_SEND) {
        seg.cmd = IKCP_CMD_WASK;

        if (ikcp_canlog(kcp, IKCP_LOG_OUT_PROBE)) {
            ikcp_log(kcp, IKCP_LOG_OUT_WINS, "output probe sn=%lu, wnd=%lu",
                    (unsigned long)seg.sn, (unsigned long)seg.wnd);
        }

        size = (int)(ptr - buffer);
        if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ptr = ikcp_encode_seg(ptr, &seg);
    }

    // flush window probing commands
    if (kcp->probe & IKCP_ASK_TELL) {
        seg.cmd = IKCP_CMD_WINS;

        if (ikcp_canlog(kcp, IKCP_LOG_OUT_WINS)) {
            ikcp_log(kcp, IKCP_LOG_OUT_WINS, "output wins sn=%lu, wnd=%lu",
                    (unsigned long)seg.sn, (unsigned long)seg.wnd);
        }

        size = (int)(ptr - buffer);
        if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ptr = ikcp_encode_seg(ptr, &seg);
    }

    kcp->probe = 0;

    // fast path
    if (ikcp_waitsnd(kcp) == 0) {
        if (kcp->reminds > 0) {
            kcp->reminds--;

            size = (int)(ptr - buffer);
            if (size == 0) {
                seg.cmd = IKCP_CMD_WINS;
                if (ikcp_canlog(kcp, IKCP_LOG_OUT_WINS)) {
                    ikcp_log(kcp, IKCP_LOG_OUT_WINS, "output wins sn=%lu, wnd=%lu",
                            (unsigned long)seg.sn, (unsigned long)seg.wnd);
                }
                ptr = ikcp_encode_seg(ptr, &seg);
            }
        }

        size = (int)(ptr - buffer);
        if (size > 0) {
            ikcp_output(kcp, buffer, size);
        }
        return;
    }

    // initcwnd
    if (kcp->cwnd < 1) {
        kcp->cwnd = IKCP_CWND_INIT;
        kcp->incr = kcp->cwnd * kcp->mss;
    }

    // calculate window size
    cwnd = _imin_(kcp->snd_wnd, kcp->rmt_wnd);
    if (kcp->nocwnd == 0) cwnd = _imin_(kcp->cwnd, cwnd);

    // move data from snd_queue to snd_buf
    while (_itimediff(kcp->snd_nxt, kcp->snd_una + cwnd) < 0) {
        IKCPSEG *newseg;
        if (iqueue_is_empty(&kcp->snd_queue)) break;

        newseg = iqueue_entry(kcp->snd_queue.next, IKCPSEG, node);

        iqueue_del(&newseg->node);
        iqueue_add_tail(&newseg->node, &kcp->snd_buf);
        kcp->nsnd_que--;
        kcp->nsnd_buf++;

        newseg->conv = kcp->conv;
        newseg->cmd = IKCP_CMD_PUSH;
        newseg->wnd = seg.wnd;
        newseg->ts = current;
        newseg->sn = kcp->snd_nxt++;
        newseg->una = kcp->rcv_nxt;
        newseg->resendts = current;
        newseg->rto = kcp->rx_rto;
        newseg->fastack = 0;
        newseg->xmit = 0;
    }

    // calculate resent
    resent = (kcp->fastresend > 0)? (IUINT32)kcp->fastresend : 0xffffffff;
    rtomin = (kcp->nodelay == 0)? (kcp->rx_rto >> 3) : 0;

    // flush data segments
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
        IKCPSEG *segment = iqueue_entry(p, IKCPSEG, node);
        int needsend = 0;
        if (segment->xmit == 0) {
            if (ikcp_quotah(kcp, segment->len) != 0) {
                // break when no quota.
                break;
            }
            needsend = 1;
            segment->xmit++;
            segment->rto = kcp->rx_rto;
            segment->resendts = current + segment->rto + rtomin;
        }
        else if (_itime64diff(current, segment->resendts) >= 0) {
            needsend = 1;
            // ikcp_quotah(kcp, segment->len); // dont break.
            segment->xmit++;
            kcp->xmit++;
            if (kcp->nodelay == 0) {
                segment->rto += _imax_(segment->rto, (IUINT32)kcp->rx_rto);
            }    else {
                IINT32 step = (kcp->nodelay < 2)?
                    ((IINT32)(segment->rto)) : kcp->rx_rto;
                segment->rto += step / 2;
            }
            segment->resendts = current + segment->rto;
            lost = 1;
            ikcp_losth(kcp, segment);
        }
        else if (segment->fastack >= resent) {
            if ((int)segment->xmit <= kcp->fastlimit ||
                kcp->fastlimit <= 0) {
                needsend = 1;
                // ikcp_quotah(kcp, segment->len); // dont break.
                segment->xmit++;
                segment->fastack = 0;
                segment->resendts = current + segment->rto;
                change++;
            }
        }

        if (needsend) {
            int need;
            segment->ts = current;
            segment->wnd = seg.wnd;
            segment->una = kcp->rcv_nxt;

            if (ikcp_canlog(kcp, IKCP_LOG_OUT_DATA)) {
                ikcp_log(kcp, IKCP_LOG_OUT_DATA, "output data sn=%lu cur=%lu",
                        (unsigned long)segment->sn, (unsigned long)current);
            }

            size = (int)(ptr - buffer);
            need = IKCP_OVERHEAD + segment->len;

            if (size + need > (int)kcp->mtu) {
                if (ikcp_output(kcp, buffer, size) != 0) {
                    // break when output failed.
                    break;
                }
                ptr = buffer;
            }

            ptr = ikcp_encode_seg(ptr, segment);

            if (segment->len > 0) {
                memcpy(ptr, segment->data, segment->len);
                ptr += segment->len;
            }

            if (segment->xmit >= kcp->dead_link) {
                kcp->state = (IUINT32)-1;
            }
        }
    }

    // flash remain segments
    size = (int)(ptr - buffer);
    if (size > 0) {
        ikcp_output(kcp, buffer, size);
    }

    // update ssthresh
    if (change) {
        IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
        kcp->ssthresh = inflight / 2;
        if (kcp->ssthresh < IKCP_THRESH_MIN)
            kcp->ssthresh = IKCP_THRESH_MIN;
        #if 0
        kcp->cwnd = kcp->ssthresh + resent;
        kcp->incr = kcp->cwnd * kcp->mss;
        #endif
    }

    if (lost) {
        kcp->ssthresh = cwnd / 2;
        if (kcp->ssthresh < IKCP_THRESH_MIN)
            kcp->ssthresh = IKCP_THRESH_MIN;
        #if 0
        kcp->cwnd = 1;
        kcp->incr = kcp->mss;
        #else
        if (kcp->cwnd > IKCP_CWND_LOW) {
            kcp->cwnd -= kcp->cwnd / 8;
            kcp->incr = kcp->cwnd * kcp->mss;
        }
        #endif
    }

    if (kcp->cwnd < IKCP_CWND_MIN) {
        kcp->cwnd = IKCP_CWND_MIN;
        kcp->incr = kcp->cwnd * kcp->mss;
    }
}


//---------------------------------------------------------------------
// Determine when should you invoke ikcp_update:
// returns when you should invoke ikcp_update in millisec, if there
// is no ikcp_input/_send calling. you can call ikcp_update in that
// time, instead of call update repeatly.
// Important to reduce unnacessary ikcp_update invoking. use it to
// schedule ikcp_update (eg. implementing an epoll-like mechanism,
// or optimize ikcp_update when handling massive kcp connections)
//---------------------------------------------------------------------
IINT64 ikcp_check(const ikcpcb *kcp, IINT64 current)
{
    IINT64 ts_flush = kcp->ts_flush;
    IINT32 tm_flush = 0x7fffffff;
    IINT32 tm_packet = 0x7fffffff;
    IINT32 minimal = 0;
    struct IQUEUEHEAD *p;

    if (kcp->updated == 0) {
        return current;
    }

    if (_itime64diff(current, ts_flush) >= 10000 ||
        _itime64diff(current, ts_flush) < -10000) {
        ts_flush = current;
    }

    if (_itime64diff(current, ts_flush) >= 0) {
        return current;
    }

    tm_flush = _itime64diff(ts_flush, current);

    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
        const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
        IINT32 diff = seg->xmit == 0 ? kcp->interval : _itime64diff(seg->resendts, current);
        if (diff <= 0) {
            return current;
        }
        if (diff < tm_packet) tm_packet = diff;
    }

    minimal = tm_packet < tm_flush ? tm_packet : tm_flush;
    if (minimal >= kcp->interval) minimal = kcp->interval;

    return current + minimal;
}


int ikcp_setmtu(ikcpcb *kcp, int mtu)
{
    char *buffer;
    if (mtu < 50 || mtu < (int)IKCP_OVERHEAD)
        return -1;
    buffer = (char*)ikcp_malloc((mtu + IKCP_OVERHEAD) * 3);
    if (buffer == NULL)
        return -2;
    kcp->mtu = mtu;
    kcp->mss = kcp->mtu - IKCP_OVERHEAD;
    ikcp_free(kcp->buffer);
    kcp->buffer = buffer;
    return 0;
}

int ikcp_interval(ikcpcb *kcp, int interval)
{
    if (interval > 5000) interval = 5000;
    else if (interval < 10) interval = 10;
    kcp->interval = interval;
    return 0;
}

int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc)
{
    if (nodelay >= 0) {
        kcp->nodelay = nodelay;
        if (nodelay) {
            kcp->rx_minrto = IKCP_RTO_NDL;
        }
        else {
            kcp->rx_minrto = IKCP_RTO_MIN;
        }
    }
    if (interval >= 0) {
        if (interval > 5000) interval = 5000;
        else if (interval < 10) interval = 10;
        kcp->interval = interval;
    }
    if (resend >= 0) {
        kcp->fastresend = resend;
    }
    if (nc >= 0) {
        kcp->nocwnd = nc;
    }
    return 0;
}


int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd)
{
    if (kcp) {
        if (sndwnd > 0) {
            kcp->snd_wnd = sndwnd;
        }
        if (rcvwnd > 0) {   // must >= max fragment size
            kcp->rcv_wnd = _imax_(rcvwnd, IKCP_WND_RCV);
        }
    }
    return 0;
}

int ikcp_waitsnd(const ikcpcb *kcp)
{
    return kcp->nsnd_buf + kcp->nsnd_que;
}

int ikcp_mayrcv(const ikcpcb *kcp)
{
    return kcp->nrcv_buf + kcp->nrcv_que;
}


// read conv
IUINT32 ikcp_getconv(const void *ptr)
{
    IUINT32 conv;
    ikcp_decode32u((const char*)ptr, &conv);
    return conv;
}

IINT64 ikcp_current(ikcpcb *kcp)
{
    return kcp->ts_current;
}

void ikcp_current_update(ikcpcb *kcp, IINT64 current)
{
    kcp->ts_current = current;
}

void ikcp_save(ikcpcb *kcp, ikcprec *rec)
{
    rec->rx_rttval = kcp->rx_rttval;
    rec->rx_srtt   = kcp->rx_srtt;
    rec->rx_rto    = kcp->rx_rto;
    rec->ssthresh  = kcp->ssthresh;
    rec->cwnd      = _ibound_(IKCP_CWND_MIN_SAVE, kcp->cwnd, IKCP_CWND_MAX_SAVE);
}

void ikcp_load(ikcpcb *kcp, ikcprec *rec)
{
    if (0 == rec->rx_srtt) {
        return;
    }
    kcp->rx_rttval = rec->rx_rttval;
    kcp->rx_srtt   = rec->rx_srtt;
    kcp->rx_rto    = rec->rx_rto;
    kcp->ssthresh  = rec->ssthresh;
    kcp->cwnd      = rec->cwnd;
}
