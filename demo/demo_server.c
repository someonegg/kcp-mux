#include "demo_common.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct demo_server_s {
    demo_endpoint_t endpoint;
    const char *host;
    unsigned short port;
    unsigned batch_threshold;
    struct demo_server_stream_s *streams;
} demo_server_t;

typedef struct demo_server_stream_s {
    demo_server_t *server;
    kcpmux_stream_t *stream;
    struct demo_server_stream_s *next;
    uint32_t stream_id;
    int readable;
    int closed;
} demo_server_stream_t;

static void demo_server_stream_read_notify(kcpmux_stream_t *stream, void *user_data);
static void demo_server_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data);
static int demo_server_process_streams(demo_server_t *server);

static void demo_server_cleanup(demo_server_t *server)
{
    demo_endpoint_cleanup(&server->endpoint);
    while (server->streams != NULL) {
        demo_server_stream_t *stream = server->streams;
        server->streams = stream->next;
        free(stream);
    }
}

static void demo_server_usage(const char *prog)
{
    fprintf(
        stderr,
        "Usage: %s [--host ADDR] [--port PORT] [--batch-threshold N] [--quiet]\n"
        "\n"
        "Defaults: --host %s --port %d --batch-threshold 4\n",
        prog,
        DEMO_DEFAULT_HOST,
        DEMO_DEFAULT_PORT);
}

static int demo_server_parse_args(int argc, char **argv, demo_server_t *server)
{
    server->host = DEMO_DEFAULT_HOST;
    server->port = DEMO_DEFAULT_PORT;
    server->batch_threshold = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            demo_server_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            server->host = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (demo_parse_port(argv[++i], &server->port) != 0) {
                fprintf(stderr, "invalid --port value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--batch-threshold") == 0 && i + 1 < argc) {
            if (demo_parse_uint(argv[++i], &server->batch_threshold, 1, UINT_MAX) != 0) {
                fprintf(stderr, "invalid --batch-threshold value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--quiet") == 0) {
            server->endpoint.quiet = 1;
            continue;
        }

        fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
        demo_server_usage(argv[0]);
        return -1;
    }

    return 0;
}

static void demo_server_conn_state_changed(
    kcpmux_conn_t *conn,
    uint8_t old_state,
    uint8_t new_state,
    void *user_data)
{
    demo_server_t *server = (demo_server_t *)user_data;
    (void)conn;
    (void)old_state;

    if (!server->endpoint.quiet) {
        printf("server connection state=%u\n", (unsigned)new_state);
        fflush(stdout);
    }
}

static void demo_server_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data)
{
    demo_server_t *server = (demo_server_t *)user_data;
    (void)conn;

    if (!server->endpoint.quiet) {
        printf("server connection closed reason=%d\n", reason);
        fflush(stdout);
    }
}

static int demo_server_stream_create_notify(kcpmux_stream_t *stream, void *user_data)
{
    demo_server_t *server = (demo_server_t *)user_data;
    demo_server_stream_t *server_stream;
    kcpmux_stream_callbacks_t callbacks;
    kcpmux_stream_config_t config;

    server_stream = (demo_server_stream_t *)calloc(1, sizeof(*server_stream));
    if (server_stream == NULL) {
        return -1;
    }

    server_stream->server = server;
    server_stream->stream = stream;
    server_stream->stream_id = kcpmux_stream_id(stream);
    server_stream->next = server->streams;
    server->streams = server_stream;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stream_read_notify = demo_server_stream_read_notify;
    callbacks.stream_close_notify = demo_server_stream_close_notify;
    kcpmux_stream_config_init(&config);
    config.batch_threshold = server->batch_threshold;
    kcpmux_stream_set_config(stream, &config);
    kcpmux_stream_set_callbacks(stream, &callbacks, server_stream);

    if (!server->endpoint.quiet) {
        printf("server accepted stream=%u\n", server_stream->stream_id);
        fflush(stdout);
    }
    return 0;
}

static int demo_server_conn_connect_notify(
    kcpmux_conn_t *conn,
    const kcpmux_proto_ext_t *proto_ext,
    kcpmux_proto_ext_t *resp_proto_ext,
    void *user_data)
{
    demo_endpoint_t *endpoint = (demo_endpoint_t *)user_data;
    demo_server_t *server = (demo_server_t *)endpoint->app_user_data;
    kcpmux_conn_callbacks_t callbacks;

    (void)proto_ext;
    (void)resp_proto_ext;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.conn_state_changed = demo_server_conn_state_changed;
    callbacks.conn_close_notify = demo_server_conn_close_notify;
    callbacks.stream_create_notify = demo_server_stream_create_notify;
    kcpmux_conn_set_callbacks(conn, &callbacks, server);

    if (!server->endpoint.quiet) {
        printf("server accepted connection\n");
        fflush(stdout);
    }

    return KCPMUX_ACK_RESULT_OK;
}

static void demo_server_stream_read_notify(kcpmux_stream_t *stream, void *user_data)
{
    demo_server_stream_t *server_stream = (demo_server_stream_t *)user_data;
    (void)stream;

    server_stream->readable = 1;
}

static int demo_server_process_streams(demo_server_t *server)
{
    demo_server_stream_t **link = &server->streams;
    uint8_t buf[4096];

    while (*link != NULL) {
        demo_server_stream_t *server_stream = *link;

        if (server_stream->closed) {
            *link = server_stream->next;
            free(server_stream);
            continue;
        }
        if (!server_stream->readable) {
            link = &server_stream->next;
            continue;
        }
        server_stream->readable = 0;

        for (;;) {
            int message_size = kcpmux_stream_peek_size(server_stream->stream);
            int received;
            int sent;

            if (message_size == 0) {
                break;
            }
            if (message_size < 0 || (size_t)message_size > sizeof(buf)) {
                return -1;
            }
            received = kcpmux_stream_recv(server_stream->stream, buf, sizeof(buf));
            if (received <= 0 || server_stream->closed) {
                return -1;
            }
            sent = kcpmux_stream_send(server_stream->stream, buf, (unsigned)received, 0);
            if (sent != received || server_stream->closed) {
                return -1;
            }

            if (!server->endpoint.quiet) {
                printf(
                    "server echo stream=%u bytes=%d sent=%d\n",
                    server_stream->stream_id,
                    received,
                    sent);
                fflush(stdout);
            }
        }
        kcpmux_stream_finish_batch(server_stream->stream);
        link = &server_stream->next;
    }

    return 0;
}

static void demo_server_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data)
{
    demo_server_stream_t *server_stream = (demo_server_stream_t *)user_data;

    (void)stream;

    if (server_stream != NULL && !server_stream->server->endpoint.quiet) {
        printf("server stream=%u closed reason=%d\n", server_stream->stream_id, reason);
        fflush(stdout);
    }
    if (server_stream != NULL) {
        server_stream->stream = NULL;
        server_stream->closed = 1;
    }
}

int main(int argc, char **argv)
{
    demo_server_t server;
    kcpmux_engine_callbacks_t callbacks;
    int parse_ret;

    memset(&server, 0, sizeof(server));
    server.endpoint.fd = -1;

    parse_ret = demo_server_parse_args(argc, argv, &server);
    if (parse_ret != 0) {
        return parse_ret > 0 ? 0 : 1;
    }

    if (demo_udp_bind(&server.endpoint.fd, server.host, server.port) != 0) {
        fprintf(stderr, "failed to bind UDP socket on %s:%hu\n", server.host, server.port);
        return 1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.conn_connect_notify = demo_server_conn_connect_notify;
    if (demo_endpoint_init(&server.endpoint, &callbacks, &server) != 0) {
        fprintf(stderr, "failed to create kcpmux engine\n");
        demo_endpoint_cleanup(&server.endpoint);
        return 1;
    }

    if (!server.endpoint.quiet) {
        printf("server listening on %s:%hu\n", server.host, server.port);
        fflush(stdout);
    }

    for (;;) {
        if (demo_endpoint_poll(&server.endpoint, 100) != 0) {
            fprintf(stderr, "server event loop failed\n");
            demo_server_cleanup(&server);
            return 1;
        }
        if (demo_server_process_streams(&server) != 0) {
            fprintf(stderr, "server failed to echo stream data\n");
            demo_server_cleanup(&server);
            return 1;
        }
    }
}
