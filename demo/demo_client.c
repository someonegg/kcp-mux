#include "demo_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_MAX_STREAMS 64
#define DEMO_MAX_MESSAGE_LEN 512
#define DEMO_PAYLOAD_LEN 1024

typedef struct demo_client_s demo_client_t;

typedef struct demo_client_stream_s {
    demo_client_t *client;
    kcpmux_stream_t *stream;
    char payload[DEMO_PAYLOAD_LEN];
    size_t payload_len;
    size_t received_len;
    int done;
} demo_client_stream_t;

struct demo_client_s {
    demo_endpoint_t endpoint;
    const char *host;
    unsigned short port;
    unsigned streams_count;
    const char *message;
    unsigned timeout_ms;
    kcpmux_conn_t *conn;
    int connected;
    int streams_started;
    demo_client_stream_t streams[DEMO_MAX_STREAMS];
};

static void demo_client_stream_read_notify(kcpmux_stream_t *stream, void *user_data);

static void
demo_client_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--host ADDR] [--port PORT] [--streams N] [--message TEXT] "
            "[--timeout-ms MS] [--quiet]\n"
            "\n"
            "Defaults: --host %s --port %d --streams 3 --message hello --timeout-ms 5000\n",
            prog, DEMO_DEFAULT_HOST, DEMO_DEFAULT_PORT);
}

static int
demo_client_parse_args(int argc, char **argv, demo_client_t *client)
{
    client->host = DEMO_DEFAULT_HOST;
    client->port = DEMO_DEFAULT_PORT;
    client->streams_count = 3;
    client->message = "hello";
    client->timeout_ms = 5000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            demo_client_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            client->host = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (demo_parse_port(argv[++i], &client->port) != 0) {
                fprintf(stderr, "invalid --port value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--streams") == 0 && i + 1 < argc) {
            if (demo_parse_uint(argv[++i], &client->streams_count, 1, DEMO_MAX_STREAMS) != 0) {
                fprintf(stderr, "invalid --streams value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) {
            client->message = argv[++i];
            if (strlen(client->message) > DEMO_MAX_MESSAGE_LEN) {
                fprintf(stderr, "--message is too long\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            if (demo_parse_uint(argv[++i], &client->timeout_ms, 100, 60000) != 0) {
                fprintf(stderr, "invalid --timeout-ms value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--quiet") == 0) {
            client->endpoint.quiet = 1;
            continue;
        }

        fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
        demo_client_usage(argv[0]);
        return -1;
    }

    return 0;
}

static void
demo_client_conn_state_changed(kcpmux_conn_t *conn, uint8_t old_state,
                               uint8_t new_state, void *user_data)
{
    demo_client_t *client = (demo_client_t *)user_data;
    (void)conn;
    (void)old_state;

    client->connected = new_state == KCPMUX_CONN_STATE_CONNECTED;
    if (!client->endpoint.quiet) {
        printf("client connection state=%u\n", (unsigned)new_state);
        fflush(stdout);
    }
}

static void
demo_client_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data)
{
    demo_client_t *client = (demo_client_t *)user_data;
    (void)conn;

    if (!client->endpoint.quiet) {
        printf("client connection closed reason=%d\n", reason);
        fflush(stdout);
    }
}

static void
demo_client_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data)
{
    demo_client_stream_t *client_stream = (demo_client_stream_t *)user_data;

    if (client_stream != NULL && !client_stream->client->endpoint.quiet) {
        printf("client stream=%u closed reason=%d\n", kcpmux_stream_id(stream), reason);
        fflush(stdout);
    }
}

static int
demo_client_connect(demo_client_t *client)
{
    struct sockaddr_in peer;
    uint8_t addr_buf[6];
    kcpmux_addr_t addr;
    kcpmux_conn_callbacks_t callbacks;

    if (demo_udp_resolve_ipv4(client->host, client->port, &peer) != 0
        || demo_sockaddr_to_kcpmux_addr(&peer, &addr, addr_buf) != 0)
    {
        fprintf(stderr, "failed to resolve %s:%hu\n", client->host, client->port);
        return -1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.conn_state_changed = demo_client_conn_state_changed;
    callbacks.conn_close_notify = demo_client_conn_close_notify;

    client->conn = kcpmux_conn_connect(client->endpoint.engine, &addr, NULL, NULL,
                                       &callbacks, client);
    return client->conn != NULL ? 0 : -1;
}

static int
demo_client_start_streams(demo_client_t *client)
{
    kcpmux_stream_callbacks_t callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stream_read_notify = demo_client_stream_read_notify;
    callbacks.stream_close_notify = demo_client_stream_close_notify;

    for (unsigned i = 0; i < client->streams_count; i++) {
        demo_client_stream_t *client_stream = &client->streams[i];
        int written;
        int sent;

        memset(client_stream, 0, sizeof(*client_stream));
        client_stream->client = client;
        written = snprintf(client_stream->payload, sizeof(client_stream->payload),
                           "stream=%u message=%s", i + 1, client->message);
        if (written < 0 || (size_t)written >= sizeof(client_stream->payload)) {
            return -1;
        }
        client_stream->payload_len = (size_t)written;

        client_stream->stream = kcpmux_stream_create(client->conn, NULL, &callbacks,
                                                     client_stream);
        if (client_stream->stream == NULL) {
            return -1;
        }

        sent = kcpmux_stream_send(client_stream->stream,
                                  (const uint8_t *)client_stream->payload,
                                  (unsigned)client_stream->payload_len, 1);
        if (sent <= 0) {
            return -1;
        }

        if (!client->endpoint.quiet) {
            printf("client sent stream=%u bytes=%d\n",
                   kcpmux_stream_id(client_stream->stream), sent);
            fflush(stdout);
        }
    }

    client->streams_started = 1;
    return 0;
}

static void
demo_client_stream_read_notify(kcpmux_stream_t *stream, void *user_data)
{
    demo_client_stream_t *client_stream = (demo_client_stream_t *)user_data;
    uint8_t buf[4096];
    int ret;

    (void)stream;

    while ((ret = kcpmux_stream_recv(client_stream->stream, buf, sizeof(buf))) > 0) {
        size_t remaining;

        if (client_stream->done) {
            continue;
        }

        remaining = client_stream->payload_len - client_stream->received_len;
        if ((size_t)ret > remaining
            || memcmp(client_stream->payload + client_stream->received_len, buf, (size_t)ret) != 0)
        {
            fprintf(stderr, "client received unexpected echo on stream=%u\n",
                    kcpmux_stream_id(client_stream->stream));
            client_stream->done = -1;
            return;
        }

        client_stream->received_len += (size_t)ret;
        if (client_stream->received_len == client_stream->payload_len) {
            client_stream->done = 1;
            if (!client_stream->client->endpoint.quiet) {
                printf("client received echo stream=%u bytes=%zu\n",
                       kcpmux_stream_id(client_stream->stream), client_stream->received_len);
                fflush(stdout);
            }
        }
    }
}

static int
demo_client_done(const demo_client_t *client)
{
    for (unsigned i = 0; i < client->streams_count; i++) {
        if (client->streams[i].done < 0) {
            return -1;
        }
        if (client->streams[i].done == 0) {
            return 0;
        }
    }
    return 1;
}

int
main(int argc, char **argv)
{
    demo_client_t client;
    kcpmux_engine_callbacks_t callbacks;
    int parse_ret;
    int64_t deadline;
    size_t bytes = 0;

    memset(&client, 0, sizeof(client));
    client.endpoint.fd = -1;

    parse_ret = demo_client_parse_args(argc, argv, &client);
    if (parse_ret != 0) {
        return parse_ret > 0 ? 0 : 1;
    }

    if (demo_udp_open_client(&client.endpoint.fd) != 0) {
        fprintf(stderr, "failed to open UDP socket\n");
        return 1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    if (demo_endpoint_init(&client.endpoint, &callbacks, &client) != 0) {
        fprintf(stderr, "failed to create kcpmux engine\n");
        demo_endpoint_cleanup(&client.endpoint);
        return 1;
    }

    if (demo_client_connect(&client) != 0) {
        fprintf(stderr, "failed to start connection\n");
        demo_endpoint_cleanup(&client.endpoint);
        return 1;
    }

    deadline = demo_monotonic_time_ms() + (int64_t)client.timeout_ms;
    while (demo_monotonic_time_ms() < deadline) {
        int done;

        if (client.connected && !client.streams_started
            && demo_client_start_streams(&client) != 0)
        {
            fprintf(stderr, "failed to start streams\n");
            demo_endpoint_cleanup(&client.endpoint);
            return 1;
        }

        done = client.streams_started ? demo_client_done(&client) : 0;
        if (done < 0) {
            demo_endpoint_cleanup(&client.endpoint);
            return 1;
        }
        if (done > 0) {
            for (unsigned i = 0; i < client.streams_count; i++) {
                bytes += client.streams[i].received_len;
            }
            printf("OK streams=%u bytes=%zu\n", client.streams_count, bytes);
            demo_endpoint_cleanup(&client.endpoint);
            return 0;
        }

        if (demo_endpoint_poll(&client.endpoint, 10) != 0) {
            fprintf(stderr, "client event loop failed\n");
            demo_endpoint_cleanup(&client.endpoint);
            return 1;
        }
    }

    fprintf(stderr, "client timed out after %u ms\n", client.timeout_ms);
    demo_endpoint_cleanup(&client.endpoint);
    return 1;
}
