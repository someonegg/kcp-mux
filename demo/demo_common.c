#include "demo_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEMO_NO_WAKEUP INT64_MAX

int demo_parse_port(const char *value, unsigned short *port)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || port == NULL) {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 || parsed > 65535) {
        return -1;
    }

    *port = (unsigned short)parsed;
    return 0;
}

int demo_parse_uint(const char *value, unsigned *out, unsigned min, unsigned max)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || out == NULL || min > max) {
        return -1;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        return -1;
    }

    *out = (unsigned)parsed;
    return 0;
}

int64_t demo_monotonic_time_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

int demo_sockaddr_to_kcpmux_addr(
    const struct sockaddr_in *addr,
    kcpmux_addr_t *out,
    uint8_t out_buf[6])
{
    uint32_t ip;
    uint16_t port;

    if (addr == NULL || out == NULL || out_buf == NULL || addr->sin_family != AF_INET) {
        return -1;
    }

    ip = ntohl(addr->sin_addr.s_addr);
    port = ntohs(addr->sin_port);
    out_buf[0] = (uint8_t)((ip >> 24) & 0xff);
    out_buf[1] = (uint8_t)((ip >> 16) & 0xff);
    out_buf[2] = (uint8_t)((ip >> 8) & 0xff);
    out_buf[3] = (uint8_t)(ip & 0xff);
    out_buf[4] = (uint8_t)((port >> 8) & 0xff);
    out_buf[5] = (uint8_t)(port & 0xff);

    out->addr = out_buf;
    out->addrlen = 6;
    return 0;
}

int demo_kcpmux_addr_to_sockaddr(const kcpmux_addr_t *addr, struct sockaddr_in *out)
{
    uint32_t ip;
    uint16_t port;

    if (addr == NULL || addr->addr == NULL || addr->addrlen < 6 || out == NULL) {
        return -1;
    }

    ip = ((uint32_t)addr->addr[0] << 24)
       | ((uint32_t)addr->addr[1] << 16)
       | ((uint32_t)addr->addr[2] << 8)
       | (uint32_t)addr->addr[3];
    port = ((uint16_t)addr->addr[4] << 8) | (uint16_t)addr->addr[5];

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(ip);
    out->sin_port = htons(port);
    return 0;
}

static int demo_set_socket_buffers(int fd)
{
    int size = DEMO_SOCKET_BUF_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0) {
        return -1;
    }
    return 0;
}

static int demo_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    return 0;
}

int demo_udp_resolve_ipv4(const char *host, unsigned short port, struct sockaddr_in *out)
{
    if (host == NULL || out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    return inet_pton(AF_INET, host, &out->sin_addr) == 1 ? 0 : -1;
}

int demo_udp_bind(int *fd, const char *host, unsigned short port)
{
    struct sockaddr_in addr;
    int reuse = 1;
    int sock;

    if (fd == NULL || demo_udp_resolve_ipv4(host, port, &addr) != 0) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (demo_set_socket_buffers(sock) != 0
        || demo_set_nonblocking(sock) != 0
        || bind(sock, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(sock);
        return -1;
    }

    *fd = sock;
    return 0;
}

int demo_udp_open_client(int *fd)
{
    int sock;

    if (fd == NULL) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    if (demo_set_socket_buffers(sock) != 0 || demo_set_nonblocking(sock) != 0) {
        close(sock);
        return -1;
    }

    *fd = sock;
    return 0;
}

void demo_udp_close(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int demo_endpoint_init(
    demo_endpoint_t *endpoint,
    kcpmux_engine_callbacks_t *callbacks,
    void *user_data)
{
    if (endpoint == NULL || callbacks == NULL) {
        return -1;
    }

    callbacks->set_timer = demo_set_timer;
    callbacks->write_socket = demo_write_socket;
    callbacks->log_write = demo_log_write;
    callbacks->monotonic_time_ms = demo_engine_time_ms;

    endpoint->app_user_data = user_data;
    endpoint->engine = kcpmux_engine_create(NULL, NULL, NULL, callbacks, endpoint, NULL);
    endpoint->next_wakeup_ms = DEMO_NO_WAKEUP;
    return endpoint->engine != NULL ? 0 : -1;
}

void demo_endpoint_cleanup(demo_endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    if (endpoint->engine != NULL) {
        endpoint->next_wakeup_ms = DEMO_NO_WAKEUP;
        kcpmux_engine_destroy(endpoint->engine);
        endpoint->engine = NULL;
    }
    demo_udp_close(&endpoint->fd);
}

int demo_endpoint_poll(demo_endpoint_t *endpoint, int max_wait_ms)
{
    uint8_t packet[DEMO_PACKET_BUF_SIZE];
    fd_set readfds;
    struct timeval tv;
    int64_t now;
    int wait_ms;
    int ret;

    if (endpoint == NULL || endpoint->fd < 0 || endpoint->engine == NULL) {
        return -1;
    }

    now = demo_monotonic_time_ms();
    wait_ms = max_wait_ms;
    if (endpoint->next_wakeup_ms <= now) {
        wait_ms = 0;
    } else {
        int64_t until_wakeup = endpoint->next_wakeup_ms - now;
        if (until_wakeup < wait_ms) {
            wait_ms = (int)until_wakeup;
        }
    }

    FD_ZERO(&readfds);
    FD_SET(endpoint->fd, &readfds);
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;

    ret = select(endpoint->fd + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }

    if (ret > 0 && FD_ISSET(endpoint->fd, &readfds)) {
        for (;;) {
            struct sockaddr_in peer;
            socklen_t peer_len = (socklen_t)sizeof(peer);
            ssize_t nread = recvfrom(
                endpoint->fd,
                packet,
                sizeof(packet),
                0,
                (struct sockaddr *)&peer,
                &peer_len);
            if (nread >= 0) {
                if (nread > 0) {
                    uint8_t addr_buf[6];
                    kcpmux_addr_t addr;
                    if (demo_sockaddr_to_kcpmux_addr(&peer, &addr, addr_buf) == 0) {
                        (void)kcpmux_engine_input(endpoint->engine, packet, (unsigned)nread, &addr);
                    }
                }
                continue;
            }
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
            break;
        }
        kcpmux_engine_finish_batch(endpoint->engine);
    }

    now = demo_monotonic_time_ms();
    if (endpoint->next_wakeup_ms <= now) {
        endpoint->next_wakeup_ms = DEMO_NO_WAKEUP;
        kcpmux_engine_update(endpoint->engine);
    }
    return 0;
}

void demo_set_timer(uint64_t wake_after_ms, void *user_data)
{
    demo_endpoint_t *endpoint = (demo_endpoint_t *)user_data;
    if (endpoint != NULL) {
        int64_t now = demo_monotonic_time_ms();
        endpoint->next_wakeup_ms = wake_after_ms > (uint64_t)(INT64_MAX - now)
            ? INT64_MAX
            : now + (int64_t)wake_after_ms;
    }
}

int demo_write_socket(const uint8_t *buf, unsigned size, const kcpmux_addr_t *addr, void *user_data)
{
    demo_endpoint_t *endpoint = (demo_endpoint_t *)user_data;
    struct sockaddr_in peer;
    ssize_t sent;

    if (endpoint == NULL || endpoint->fd < 0 || buf == NULL
        || demo_kcpmux_addr_to_sockaddr(addr, &peer) != 0)
    {
        return 0;
    }

    sent = sendto(endpoint->fd, buf, size, 0, (const struct sockaddr *)&peer, sizeof(peer));
    return sent == (ssize_t)size ? 1 : 0;
}

int64_t demo_engine_time_ms(void *user_data)
{
    (void)user_data;
    return demo_monotonic_time_ms();
}

void demo_log_write(int level, const char *buf, unsigned size, void *user_data)
{
    demo_endpoint_t *endpoint = (demo_endpoint_t *)user_data;
    if (endpoint != NULL && endpoint->quiet) {
        return;
    }

    (void)fprintf(stderr, "[kcpmux:%d] ", level);
    (void)fwrite(buf, 1, size, stderr);
    (void)fputc('\n', stderr);
}
