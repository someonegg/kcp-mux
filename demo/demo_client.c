#include "demo_common.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_MAX_STREAMS 64
#define DEMO_MAX_MESSAGES 128
#define DEMO_MAX_MESSAGE_LEN 512
#define DEMO_PAYLOAD_LEN 1024

typedef struct demo_client_s demo_client_t;

typedef struct demo_client_stream_s {
    demo_client_t *client;
    kcpmux_stream_t *stream;
    unsigned logical_stream;
    unsigned received_messages;
    size_t received_bytes;
    uint32_t stream_id;
    int readable;
    int done;
} demo_client_stream_t;

struct demo_client_s {
    demo_endpoint_t endpoint;
    const char *host;
    unsigned short port;
    unsigned streams_count;
    unsigned messages_count;
    unsigned batch_threshold;
    const char *message;
    unsigned timeout_ms;
    kcpmux_conn_t *conn;
    int connected;
    int streams_started;
    demo_client_stream_t streams[DEMO_MAX_STREAMS];
};

static void demo_client_stream_read_notify(kcpmux_stream_t *stream, void *user_data);
static int demo_client_process_reads(demo_client_t *client);

static void demo_client_usage(const char *prog)
{
    fprintf(
        stderr,
        "Usage: %s [--host ADDR] [--port PORT] [--streams N] [--messages N] "
        "[--batch-threshold N] [--message TEXT] [--timeout-ms MS] [--quiet]\n"
        "\n"
        "Defaults: --host %s --port %d --streams 3 --messages 10 "
        "--batch-threshold 4 --message hello --timeout-ms 5000\n",
        prog,
        DEMO_DEFAULT_HOST,
        DEMO_DEFAULT_PORT);
}

static int demo_client_parse_args(int argc, char **argv, demo_client_t *client)
{
    client->host = DEMO_DEFAULT_HOST;
    client->port = DEMO_DEFAULT_PORT;
    client->streams_count = 3;
    client->messages_count = 10;
    client->batch_threshold = 4;
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
        if (strcmp(argv[i], "--messages") == 0 && i + 1 < argc) {
            if (demo_parse_uint(argv[++i], &client->messages_count, 1, DEMO_MAX_MESSAGES) != 0) {
                fprintf(stderr, "invalid --messages value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--batch-threshold") == 0 && i + 1 < argc) {
            if (demo_parse_uint(argv[++i], &client->batch_threshold, 1, UINT_MAX) != 0) {
                fprintf(stderr, "invalid --batch-threshold value\n");
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

static void demo_client_conn_state_changed(
    kcpmux_conn_t *conn,
    uint8_t old_state,
    uint8_t new_state,
    void *user_data)
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

static void demo_client_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data)
{
    demo_client_t *client = (demo_client_t *)user_data;
    (void)conn;

    client->conn = NULL;
    client->connected = 0;

    if (!client->endpoint.quiet) {
        printf("client connection closed reason=%d\n", reason);
        fflush(stdout);
    }
}

static void demo_client_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data)
{
    demo_client_stream_t *client_stream = (demo_client_stream_t *)user_data;

    (void)stream;

    if (client_stream != NULL && !client_stream->client->endpoint.quiet) {
        printf("client stream=%u closed reason=%d\n", client_stream->stream_id, reason);
        fflush(stdout);
    }
    if (client_stream != NULL) {
        client_stream->stream = NULL;
        if (!client_stream->done) {
            client_stream->done = -1;
        }
    }
}

static int demo_client_connect(demo_client_t *client)
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

    client->conn = kcpmux_conn_connect(
        client->endpoint.engine,
        &addr,
        NULL,
        NULL,
        &callbacks,
        client);
    return client->conn != NULL ? 0 : -1;
}

static int demo_client_start_streams(demo_client_t *client)
{
    kcpmux_stream_callbacks_t callbacks;
    kcpmux_stream_config_t config;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stream_read_notify = demo_client_stream_read_notify;
    callbacks.stream_close_notify = demo_client_stream_close_notify;
    kcpmux_stream_config_init(&config);
    config.batch_threshold = client->batch_threshold;

    for (unsigned i = 0; i < client->streams_count; i++) {
        demo_client_stream_t *client_stream = &client->streams[i];
        char payload[DEMO_PAYLOAD_LEN];

        memset(client_stream, 0, sizeof(*client_stream));
        client_stream->client = client;
        client_stream->logical_stream = i + 1;

        client_stream->stream = kcpmux_stream_create(client->conn, &config, &callbacks, client_stream);
        if (client_stream->stream == NULL) {
            return -1;
        }
        client_stream->stream_id = kcpmux_stream_id(client_stream->stream);

        for (unsigned message = 1; message <= client->messages_count; message++) {
            int written = snprintf(
                payload,
                sizeof(payload),
                "stream=%u message=%u text=%s",
                client_stream->logical_stream,
                message,
                client->message);
            int sent;

            if (written < 0 || (size_t)written >= sizeof(payload)) {
                return -1;
            }
            sent = kcpmux_stream_send(
                client_stream->stream, (const uint8_t *)payload, (unsigned)written, 0);
            if (sent != written) {
                return -1;
            }
            if (!client->endpoint.quiet) {
                printf("client sent stream=%u message=%u bytes=%d\n",
                    client_stream->stream_id, message, sent);
                fflush(stdout);
            }
        }
        kcpmux_stream_finish_batch(client_stream->stream);
    }

    client->streams_started = 1;
    return 0;
}

static void demo_client_stream_read_notify(kcpmux_stream_t *stream, void *user_data)
{
    demo_client_stream_t *client_stream = (demo_client_stream_t *)user_data;
    (void)stream;

    client_stream->readable = 1;
}

static int demo_client_process_reads(demo_client_t *client)
{
    uint8_t buf[4096];

    for (unsigned i = 0; i < client->streams_count; i++) {
        demo_client_stream_t *client_stream = &client->streams[i];

        if (!client_stream->readable || client_stream->stream == NULL) {
            continue;
        }
        client_stream->readable = 0;

        for (;;) {
            int message_size = kcpmux_stream_peek_size(client_stream->stream);
            int ret;
            char expected[DEMO_PAYLOAD_LEN];
            int expected_len;

            if (message_size == 0) {
                break;
            }
            if (message_size < 0 || (size_t)message_size > sizeof(buf)) {
                return -1;
            }
            ret = kcpmux_stream_recv(client_stream->stream, buf, sizeof(buf));
            if (ret <= 0) {
                return -1;
            }

            if (client_stream->done) {
                fprintf(stderr, "client received extra echo on stream=%u\n",
                    client_stream->stream_id);
                return -1;
            }

            expected_len = snprintf(expected, sizeof(expected),
                "stream=%u message=%u text=%s",
                client_stream->logical_stream,
                client_stream->received_messages + 1,
                client->message);
            if (expected_len < 0 || (size_t)expected_len >= sizeof(expected)
                || ret != expected_len || memcmp(expected, buf, (size_t)ret) != 0) {
                fprintf(
                    stderr,
                    "client received unexpected echo on stream=%u\n",
                    client_stream->stream_id);
                client_stream->done = -1;
                return -1;
            }

            client_stream->received_messages++;
            client_stream->received_bytes += (size_t)ret;
            if (client_stream->received_messages == client->messages_count) {
                client_stream->done = 1;
                if (!client->endpoint.quiet) {
                    printf(
                        "client received echo stream=%u messages=%u bytes=%zu\n",
                        client_stream->stream_id,
                        client_stream->received_messages,
                        client_stream->received_bytes);
                    fflush(stdout);
                }
            }
        }
    }

    return 0;
}

static int demo_client_done(const demo_client_t *client)
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

int main(int argc, char **argv)
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
                bytes += client.streams[i].received_bytes;
            }
            printf("OK streams=%u messages=%u bytes=%zu\n",
                client.streams_count, client.messages_count, bytes);
            demo_endpoint_cleanup(&client.endpoint);
            return 0;
        }

        if (demo_endpoint_poll(&client.endpoint, 10) != 0) {
            fprintf(stderr, "client event loop failed\n");
            demo_endpoint_cleanup(&client.endpoint);
            return 1;
        }
        if (demo_client_process_reads(&client) != 0) {
            fprintf(stderr, "client failed to receive echo\n");
            demo_endpoint_cleanup(&client.endpoint);
            return 1;
        }
    }

    fprintf(stderr, "client timed out after %u ms\n", client.timeout_ms);
    demo_endpoint_cleanup(&client.endpoint);
    return 1;
}
