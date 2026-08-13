#ifndef KCPMUX_DEMO_COMMON_H
#define KCPMUX_DEMO_COMMON_H

#include <stdint.h>

#include <netinet/in.h>

#include <kcpmux/kcpmux.h>

#define DEMO_DEFAULT_HOST "127.0.0.1"
#define DEMO_DEFAULT_PORT 8443
#define DEMO_PACKET_BUF_SIZE 65535
#define DEMO_SOCKET_BUF_SIZE (1024 * 1024)

typedef struct demo_endpoint_s {
    int fd;
    kcpmux_engine_t *engine;
    void *app_user_data;
    int64_t next_wakeup_ms;
    int quiet;
} demo_endpoint_t;

int demo_parse_port(const char *value, unsigned short *port);
int demo_parse_uint(const char *value, unsigned *out, unsigned min, unsigned max);
int64_t demo_monotonic_time_ms(void);

int demo_sockaddr_to_kcpmux_addr(
    const struct sockaddr_in *addr,
    kcpmux_addr_t *out,
    uint8_t out_buf[6]);
int demo_kcpmux_addr_to_sockaddr(const kcpmux_addr_t *addr, struct sockaddr_in *out);

int demo_udp_bind(int *fd, const char *host, unsigned short port);
int demo_udp_open_client(int *fd);
int demo_udp_resolve_ipv4(const char *host, unsigned short port, struct sockaddr_in *out);
void demo_udp_close(int *fd);

int demo_endpoint_init(
    demo_endpoint_t *endpoint,
    kcpmux_engine_callbacks_t *callbacks,
    void *user_data);
void demo_endpoint_cleanup(demo_endpoint_t *endpoint);
int demo_endpoint_poll(demo_endpoint_t *endpoint, int max_wait_ms);

void demo_set_timer(uint64_t wake_after_ms, void *user_data);
int demo_write_socket(
    const uint8_t *buf,
    unsigned size,
    const kcpmux_addr_t *addr,
    void *user_data);
int64_t demo_engine_time_ms(void *user_data);
void demo_log_write(int level, const char *buf, unsigned size, void *user_data);

#endif
