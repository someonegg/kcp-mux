#include "demo_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct demo_server_s {
    demo_endpoint_t endpoint;
    const char *host;
    unsigned short port;
} demo_server_t;

typedef struct demo_server_stream_s {
    demo_server_t *server;
    kcpmux_stream_t *stream;
} demo_server_stream_t;

static void demo_server_stream_read_notify(kcpmux_stream_t *stream, void *user_data);
static void demo_server_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data);

static void
demo_server_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--host ADDR] [--port PORT] [--quiet]\n"
            "\n"
            "Defaults: --host %s --port %d\n",
            prog, DEMO_DEFAULT_HOST, DEMO_DEFAULT_PORT);
}

static int
demo_server_parse_args(int argc, char **argv, demo_server_t *server)
{
    server->host = DEMO_DEFAULT_HOST;
    server->port = DEMO_DEFAULT_PORT;

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

static void
demo_server_conn_state_changed(kcpmux_conn_t *conn, uint8_t old_state,
                               uint8_t new_state, void *user_data)
{
    demo_server_t *server = (demo_server_t *)user_data;
    (void)conn;
    (void)old_state;

    if (!server->endpoint.quiet) {
        printf("server connection state=%u\n", (unsigned)new_state);
        fflush(stdout);
    }
}

static void
demo_server_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data)
{
    demo_server_t *server = (demo_server_t *)user_data;
    (void)conn;

    if (!server->endpoint.quiet) {
        printf("server connection closed reason=%d\n", reason);
        fflush(stdout);
    }
}

static void
demo_server_stream_create_notify(kcpmux_stream_t *stream, void *user_data)
{
    demo_server_t *server = (demo_server_t *)user_data;
    demo_server_stream_t *server_stream;
    kcpmux_stream_callbacks_t callbacks;

    server_stream = (demo_server_stream_t *)calloc(1, sizeof(*server_stream));
    if (server_stream == NULL) {
        return;
    }

    server_stream->server = server;
    server_stream->stream = stream;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stream_read_notify = demo_server_stream_read_notify;
    callbacks.stream_close_notify = demo_server_stream_close_notify;
    kcpmux_stream_set_callbacks(stream, &callbacks, server_stream);

    if (!server->endpoint.quiet) {
        printf("server accepted stream=%u\n", kcpmux_stream_id(stream));
        fflush(stdout);
    }
}

static int
demo_server_conn_connect_notify(kcpmux_conn_t *conn,
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

static void
demo_server_stream_read_notify(kcpmux_stream_t *stream, void *user_data)
{
    demo_server_stream_t *server_stream = (demo_server_stream_t *)user_data;
    uint8_t buf[4096];
    int ret;

    while ((ret = kcpmux_stream_recv(stream, buf, sizeof(buf))) > 0) {
        int sent = kcpmux_stream_send(stream, buf, (unsigned)ret, 1);
        if (!server_stream->server->endpoint.quiet) {
            printf("server echo stream=%u bytes=%d sent=%d\n",
                   kcpmux_stream_id(stream), ret, sent);
            fflush(stdout);
        }
    }
}

static void
demo_server_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data)
{
    demo_server_stream_t *server_stream = (demo_server_stream_t *)user_data;

    if (server_stream != NULL && !server_stream->server->endpoint.quiet) {
        printf("server stream=%u closed reason=%d\n", kcpmux_stream_id(stream), reason);
        fflush(stdout);
    }

    free(server_stream);
}

int
main(int argc, char **argv)
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
            demo_endpoint_cleanup(&server.endpoint);
            return 1;
        }
    }
}
